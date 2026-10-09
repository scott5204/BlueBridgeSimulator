#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

#include <QElapsedTimer>
#include <QFile>
#include <QObject>
#include <QString>
#include <QTimer>

#include "board/Ct117eM4.h"
#include "cpu/ICpuEngine.h"
#include "cpu/UnicornCpu.h"
#include "debug/DebugTypes.h"
#include "sim/SimSnapshot.h"
#include "stm32/Stm32G431.h"

// Cheap per-phase timer for the batch diagnostics (Simulator.cpp).
using PerfClock = std::chrono::steady_clock;

// ============================================================================
// Simulation orchestrator.
//
// Owns the CPU engine (QEMU/unicorn Cortex-M4), the STM32G431 SoC model and
// the CT117E-M4 board model.
//
// THREADING NOTE: the unicorn 2.1.4 Windows DLL -- both the MSVC CRL and the
// MSYS2 MinGW-ucrt builds -- CRASHES on uc_hook_add()/uc_ctl() when called
// from any worker thread (access violation inside uc_vmem_write). Only the
// thread that created the engine is safe. Therefore this class is
// SINGLE-THREADED: the engine is created, and every batch is executed, on
// the object's own thread (the GUI thread). The run loop is driven by a
// QTimer with short, non-blocking batches so the GUI stays responsive.
//
// Execution model (per QTimer tick):
//   run batches of up to kMaxBatchInsns instructions, then:
//     1. advance the virtual clock (SysTick counting + IRQ pending)
//     2. inject pending Cortex-M exceptions (manual NVIC entry frame)
//     3. handle exception returns (EXC_RETURN magic fetch)
//     4. emit a throttled state snapshot
//
// Virtual clock: 1 executed instruction ~ 1 HCLK cycle (the real G431 runs
// flash with wait states, so the simulation is marginally optimistic).
// ============================================================================
class Simulator : public QObject, public ICpuHost {
    Q_OBJECT
public:
    explicit Simulator(QObject* parent = nullptr);
    ~Simulator() override;

    // ---- control (call on the same thread as the run loop) ----
    void startRun();                 // resume / start execution
    void pause();                    // pause execution
    bool isRunning() const { return running_.load(); }
    bool firmwareLoaded() const { return firmwareLoaded_.load(); }

    void setButton(int index, bool pressed);  // board button input
    // NOTE: there is deliberately no speed setting. The simulation always runs
    // 1:1 with wall time (a firmware delay(1000) is one real second).

    // ---- board inputs: pulse source + potentiometers (analog panel) ----
    // The GUI may configure these three board-level sources and nothing else:
    // it can never write a timer register, an ADC register or firmware RAM.
    //   PA15 pulse input  -> real edges -> GPIO AF -> TIM capture
    //   PB4  pulse input  -> real edges -> GPIO AF -> TIM16/TIM3 capture
    //   R37 / R38         -> analog voltage -> ADC channel -> ADC_DR
    void setPulseEnabled(bool enabled);
    void setPulseFrequency(double hz);
    void setPb4PulseEnabled(bool enabled);
    void setPb4PulseFrequency(double hz);
    void setR37Voltage(double volts) { board_->setR37Voltage(volts); }
    void setR38Voltage(double volts) { board_->setR38Voltage(volts); }

    // ---- serial terminal: the board's virtual PC peer (USART1 PA9/PA10) ----
    // The GUI may only TYPE bytes into the terminal and READ what the firmware
    // transmitted; it can never touch a USART register. The bytes travel over
    // the board's serial line, so a firmware with a wrong AF pin really
    // receives/sends nothing.
    void sendUsart1Bytes(const std::vector<uint8_t>& bytes);
    // Bytes the MCU sent since the last call (never the whole history).
    std::vector<uint8_t> takeUsart1TxBytes() {
        return board_->serialTakeFromMcu();
    }

    // Board input ranges for the GUI knobs (never hard-coded in the GUI).
    BoardProfile boardProfile() const { return board_->profile(); }

    // Current PA7 (PWM output) waveform as measured at the pin.
    DigitalSignalMonitor::Stats pa7Waveform() const {
        return board_->pa7Waveform();
    }

    // ---- LCD module (board device) ----
    // Copy the panel framebuffer (RGB565, panel order 320x240) out of the
    // board model. The GUI never touches the LcdController itself.
    void copyLcdFramebuffer(std::vector<uint16_t>& out) const {
        board_->lcd().copyPanelBuffer(out);
    }
    // Called by the GUI once it has consumed the frame (see snapshot
    // SimSnapshot::lcdDirty).
    void clearLcdDirty() { board_->lcd().clearDirty(); }
    // Development tracing: command trace on by default off; pixel trace
    // produces one line per GRAM pixel -- never enable it globally.
    void setLcdTrace(bool commands, bool pixels) {
        board_->lcd().setTraceCommands(commands);
        board_->lcd().setTracePixels(pixels);
    }

    // Diagnostics (test/debug only): peek a R4-aligned SRAM word.
    bool debugSramWord(uint32_t addr, uint32_t& out) const {
        return soc_->readWord(addr, out);
    }
    // Diagnostics (test/debug only): read a peripheral register through the
    // normal MMIO path (no side effects worth worrying about: reads only).
    uint32_t debugMmioRead(uint32_t addr) { return soc_->mmioRead(addr, 4); }
    // Diagnostics (test/debug only): poke a R4-aligned SRAM word. Used by the
    // test firmware harness to publish its command word; it is NOT a
    // peripheral backdoor (the firmware still drives every TIM register).
    bool debugSramWrite(uint32_t addr, uint32_t value) {
        return soc_->writeWord(addr, value);
    }

    // ======================================================================
    // Debug target support (src/debug/SimulatorDebugTarget + headless tests).
    // Every call must be made on the object's own thread (see the threading
    // note above), which is asserted by SimulatorDebugTarget.
    // ======================================================================
    // Guest memory with exactly the semantics the CPU sees: flash (incl. the
    // 0x00000000 boot alias), SRAM and CCM byte-for-byte; the peripheral space
    // through the normal bus model (a debug read may therefore have the same
    // side effects as a CPU read -- documented in PROJECT_HANDOFF.md).
    DebugStatus guestRead(uint32_t addr, void* data, size_t size) {
        return soc_->guestRead(addr, data, size);
    }
    DebugStatus guestWrite(uint32_t addr, const void* data, size_t size) {
        return soc_->guestWrite(addr, data, size);
    }

    // Checked CPU register access for the debug target. The DebugRegister ->
    // CpuReg mapping lives in the debug adapter (src/debug/), so no engine
    // register constant ever reaches a frontend.
    bool cpuReadRegister(CpuReg reg, uint32_t& value) {
        return cpu_->readRegister(reg, value);
    }
    bool cpuWriteRegister(CpuReg reg, uint32_t value) {
        return cpu_->writeRegister(reg, value);
    }

    // Exactly ONE guest instruction through the *identical* batch pipeline
    // (only from Halted; the breakpoint on the current PC is skipped for that
    // instruction, classic step semantics).
    bool debugStepOne();
    // Reset through the existing reset path, leaving the target Halted with
    // StopReason::Reset (vector table -> MSP/PC).
    bool debugResetHalt();
    // Let the instruction at the current PC run once, ignoring a breakpoint
    // sitting exactly there (continue/step over a breakpoint).
    void debugSkipCurrentBreakpointOnce();

    // Execution breakpoints (host-side; the guest flash bytes are never
    // patched). Addresses are Thumb-normalized (bit0 cleared) everywhere.
    bool debugAddBreakpoint(uint32_t address);     // false = already present
    bool debugRemoveBreakpoint(uint32_t address);  // false = not present
    bool debugHasBreakpoint(uint32_t address) const;
    void debugClearBreakpoints();
    const std::vector<uint32_t>& breakpoints() const { return breakpoints_; }

    // Single source of truth for the target state: why it last stopped.
    StopInfo lastStopInfo() const { return lastStop_; }
    bool cpuFaulted() const { return cpuFaulted_; }
    // Debugger event log ([debug] ...): called on state changes only, never
    // per instruction.
    void debugLogLine(const QString& text) { log("[debug] " + text); }
    // Session log line from the debug layer (e.g. "[program] begin ...").
    // Owner thread only (the file logger is single-threaded).
    void debugLog(const QString& text) { log(text); }

    // ======================================================================
    // Virtual flash programming (stage 7-2A). The debugger programming path
    // never touches the flash backing directly: it reads the image, builds a
    // staging copy and commits it through the SAME replace/reset path the
    // firmware loader uses (loadFirmware -> replaceFlashImage).
    // ======================================================================
    static constexpr size_t flashImageSize() { return Stm32G431::kFlashSize; }
    // Copy of the live flash image exactly as the CPU sees it (128 KiB).
    bool readFlashImage(std::vector<uint8_t>& out);
    // Atomic commit of a complete 128 KiB image: replaces the single live flash
    // backing store, drops the translated code, resets the SoC/board/CPU and
    // leaves the target Halted with StopReason::Reset (SP/PC are taken from the
    // NEW vector table). Auto-pauses a running target first.
    bool replaceFlashImage(const std::vector<uint8_t>& image);

    // Static memory-map facts of the modelled MCU (read from the SoC constants
    // -- never hard-coded a second time in the debug layer).
    struct MemoryMapInfo {
        uint32_t flashBase, flashSize, aliasBase, aliasSize;
        uint32_t sramBase, sramSize, ccmBase, ccmSize;
    };
    static MemoryMapInfo memoryMap();
    // Virtual HCLK cycles since the last reset (the model's time base).
    uint64_t virtualCycles() const { return soc_->totalCycles(); }

    // ======================================================================
    // Debug IPC support (stage 7-2A)
    // ======================================================================
    // "The target just stopped" observer: called on the owner thread from
    // setStop() for every non-None reason (breakpoint / user halt / single
    // step / reset / fault). The Debug IPC server turns this into
    // TARGET_STOPPED / TARGET_FAULTED events; the GUI does not use it. The
    // stop event therefore always comes from the Simulator's StopInfo -- the
    // IPC layer never guesses a reason from the command it just sent.
    void setStopObserver(std::function<void(const StopInfo&, uint64_t)> fn) {
        stopObserver_ = std::move(fn);
    }
    // Exact single-step support of the loaded engine build (probed once at
    // startup, while the translation cache is still empty).
    bool exactStepSupported() const { return exactStepSupported_; }
    // --wait-debugger: Run is suppressed until a debugger session connects.
    void setWaitForDebugger(bool on) { waitForDebugger_.store(on); }
    bool waitForDebugger() const { return waitForDebugger_.load(); }
    // Any-thread safe: atomic flag only. Set by the IPC pipe thread when a
    // session completes HELLO / disconnects; read by the owner thread in
    // startRun() -- it never touches unicorn or Qt.
    void setDebuggerAttached(bool on) { debuggerAttached_.store(on); }
    bool debuggerAttached() const { return debuggerAttached_.load(); }

public slots:
    // invoked on the object's thread (the GUI thread); returns false when the
    // image could not be loaded (the GUI uses the queued form and ignores it,
    // the debug target / tests use the return value)
    bool loadFirmware(const QString& path);
    void slotReset();
    void slotStep(int insnCount = 1);
    void toggleBreakpoint(quint32 address);
    void clearBreakpoints();

signals:
    void stateChanged(const SimSnapshot& snap);
    void logMessage(const QString& msg);
    void firmwareLoadedSig(const QString& summary);
    void firmwareLoadFailed(const QString& error);
    void runningChanged(bool running);
    void faulted(const QString& error);
    void breakpointHit(quint32 address);

private slots:
    void runTick();   // QTimer-driven: run a short batch budget

private:
    // ICpuHost (called from unicorn memory hooks during runTick)
    uint32_t cpuMmioRead(uint32_t addr, uint32_t size) override;
    void cpuMmioWrite(uint32_t addr, uint32_t value, uint32_t size) override;
    bool cpuFetchUnmapped(uint64_t addr) override;
    void cpuIntrEvent(int intno) override;

    void runOneBatch();            // one batch, budget = next event slice
    void runOneInstructionBatch(); // one batch, budget = 1 (debug single step)
    // Enable/disable the engine's exact single-step mode: every translated
    // block then holds at most one guest instruction (see UnicornCpu::
    // setExactSingleStep), which makes every count=1 batch EXACT -- the
    // guarantee the precise debugger paths need. Only the untouched fast run
    // path (runOneBatch without breakpoints) disables it again; the engine
    // flushes its translation cache only when the mode actually changes.
    void setExactStepMode(bool enable);
    // Stage 7-1 plan B: with at least one execution breakpoint the batch runs
    // one guest instruction at a time and checks the breakpoint set on the
    // host BEFORE each instruction.
    void runBreakpointBatch(uint32_t budget, PerfClock::time_point t0,
                            PerfClock::time_point t1, PerfClock::time_point t2,
                            uint64_t mmioBefore);
    // Shared batch body: executes @budget instructions, advances the virtual
    // clock, handles EXC_RETURN / fault / reset requests and services pending
    // interrupts. Used by the interactive run loop, the precise-breakpoint
    // batch and the debug single step, so there is exactly ONE execution path.
    // Returns false when the caller's batch loop must stop.
    bool executeBatch(uint32_t budget, PerfClock::time_point t0,
                      PerfClock::time_point t1, PerfClock::time_point t2,
                      uint64_t mmioBefore);
    void serviceInterrupts();
    // Stops the run loop without recording a stop reason: the internal stop
    // paths (breakpoint / fault / reset) call this and then setStop() their own
    // reason, so exactly ONE stop event is published per stop. pause() is the
    // user-facing halt (pauseImpl() + StopReason::UserHalt).
    void pauseImpl();
    void rebuildCpu();
    bool doReset();
    void doExceptionEntry(int exceptionNumber);
    void doExceptionReturn(uint32_t excReturn);
    int selectPendingException();
    void afterBatchSync();
    void setStop(StopReason reason, uint32_t pc);
    SimSnapshot makeSnapshot() const;
    // Every line is mirrored to <exe dir>/bluesim.log, flushed immediately, so
    // the diagnostics survive a freeze (the GUI log panel is on another tab and
    // cannot be clicked while the UI is stuck).
    void log(const QString& msg) {
        writeLogFile(msg);
        emit logMessage(msg);
    }
    void writeLogFile(const QString& msg);
    QFile logFile_;              // <exe dir>/bluesim.log, opened lazily
    qint64 lastPerfFileMs_ = 0;  // last baseline line written to the file

    // ---- performance diagnostics (cheap, always on) -----------------------
    // Reports one [perf] line per second into the GUI log whenever the
    // simulation is NOT keeping up (real time factor < 0.9, a tick that blocked
    // the GUI > 50 ms, dense MMIO polling) plus an immediate line for a single
    // batch that took longer than 300 ms. This is the instrument to find out
    // WHERE a freeze comes from: the batch phase breakdown separates board
    // inputs, event scheduling, the CPU batch and the peripheral advance, and
    // the MMIO rate exposes firmware that polls a register in a tight loop.
    void perfReportTick(qint64 tickMs);
    qint64 perfWindowMs_ = 0;      // start of the report window
    uint64_t perfMmio_ = 0;        // MMIO accesses (read+write) since reset
    uint64_t perfMmioWindow_ = 0;  // ... at the window start
    uint64_t perfCyclesWindow_ = 0;
    int perfTicks_ = 0, perfBatches_ = 0;
    qint64 perfTickMaxMs_ = 0;
    qint64 perfWorstUs_ = 0;       // worst single batch this window
    qint64 perfWorstPhaseUs_[4] = {0, 0, 0, 0};
    uint64_t perfWorstMmio_ = 0;
    uint64_t perfMinSlice_ = ~0ull;        // tightest batch slice this window
    const char* perfMinSliceName_ = "none";  // ... and which source caused it
    uint64_t perfExcWindow_ = 0;           // exceptionReturns_ at window start
    double realTimeX_ = 0.0;       // last window: virtual ms / wall ms

    std::unique_ptr<Stm32G431> soc_;
    std::unique_ptr<Ct117eM4> board_;
    std::unique_ptr<ICpuEngine> cpu_;
    bool engineReady_ = false;  // created once on the main thread

    // engine thread (all mutable state below is engine-thread owned except
    // the atomics)
    std::vector<int> activeExceptionStack_;
    int activeException_ = 0;
    uint8_t activePriorityByte_ = 0;
    uint64_t exceptionReturns_ = 0;
    uint64_t executedInsns_ = 0;
    std::vector<uint32_t> breakpoints_;
    // Last batch's engine stop reason (debug step bookkeeping)
    CpuStopReason lastBatchReason_ = CpuStopReason::None;
    // engine-side exact single-step mode state (precise debugger paths only)
    bool exactStepMode_ = false;
    bool exactStepWarned_ = false;  // "mode unavailable" logged once
    // one-shot breakpoint skip: "let the instruction at this PC run once"
    // (continue/step over a breakpoint sitting exactly on the current PC)
    bool skipOnceValid_ = false;
    uint32_t skipOncePc_ = 0;
    // why the target last stopped / whether it died on an unrecoverable fault:
    // the SINGLE source of truth for TargetState + StopInfo (there is no second
    // state copy inside SimulatorDebugTarget)
    StopInfo lastStop_;
    bool cpuFaulted_ = false;
    // stage 7-2A: stop observer (Debug IPC events) + engine capability probe
    std::function<void(const StopInfo&, uint64_t)> stopObserver_;
    bool exactStepSupported_ = false;

    // single-threaded state (object thread = GUI thread)
    std::atomic<bool> running_{false};
    std::atomic<bool> firmwareLoaded_{false};
    // --wait-debugger / IPC attach state (atomics: written by the pipe thread)
    std::atomic<bool> waitForDebugger_{false};
    std::atomic<bool> debuggerAttached_{false};
    double pulseHz_ = 0.0, pb4Hz_ = 0.0;  // logged with the enable transitions

    QTimer* tickTimer_ = nullptr;
    QElapsedTimer wall_;
    uint64_t runStartCycles_ = 0;  // 1:1 pacing reference (see startRun)
    qint64 lastSnapMs_ = 0;
};
