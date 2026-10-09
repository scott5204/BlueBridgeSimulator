#include "sim/Simulator.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMetaObject>

#include <algorithm>
#include <chrono>

using CpuRegId = CpuReg;

// instruction budget of one emulation batch
static constexpr uint32_t kMaxBatchInsns = 40000;
// minimum batch (keeps overhead sane when events are dense)
static constexpr uint32_t kMinBatchInsns = 64;
// Run-loop pacing (runTick). The virtual progress of one GUI tick is bounded by
// WALL TIME, never by a batch count: the board's signal sources slice every
// batch at their next edge and the serial line at every frame transition, so
// with dense events the batches get small (two 10 kHz generators -> an edge
// every 4000 cycles, 100 kHz -> every 400). A fixed "8 batches per tick" would
// then advance only tens of microseconds of VIRTUAL time per 2 ms tick and the
// whole simulation looks frozen; a wall clock budget instead uses whatever the
// host can do and never blocks the UI for long.
static constexpr int kMaxBatchesPerTick = 50000;  // safety cap only
static constexpr qint64 kTickWallBudgetMs = 2;    // == tickTimer_ interval
// a single batch that takes longer than this is logged immediately (it is the
// smoking gun when the GUI looks frozen)
static constexpr qint64 kSlowBatchWarnUs = 300000;
// 1:1 real-time pacing (see runTick): when the virtual clock is more than
// kCatchUpThresholdMs behind the wall clock (a heavy boot/MMIO burst), a tick
// may work for up to kCatchUpWindowMs instead of the normal 2 * 2 ms.
static constexpr qint64 kCatchUpThresholdMs = 20;
static constexpr qint64 kCatchUpWindowMs = 10;
// Precise-breakpoint mode (stage 7-1 plan B): instructions per batch when at
// least one execution breakpoint is set. Kept small so a batch never blocks the
// GUI: every instruction in this mode is a full single-instruction batch.
static constexpr uint32_t kPreciseBatchInsns = 64;

// cheap per-phase timer for the batch diagnostics (Simulator::PerfClock)
static inline qint64 usBetween(PerfClock::time_point a, PerfClock::time_point b) {
    return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
}

Simulator::Simulator(QObject* parent) : QObject(parent) {
    qRegisterMetaType<SimSnapshot>("SimSnapshot");

    soc_ = std::make_unique<Stm32G431>();
    board_ = std::make_unique<Ct117eM4>(*soc_);
    cpu_ = std::make_unique<UnicornCpu>();

    soc_->setLogger([this](const std::string& s) {
        log(QString::fromStdString(s));
    });
    soc_->itm.textSink = [this](const char* s) {
        log(QString("[itm] ") + s);
    };
    soc_->ppb.cpacrChanged = [this](uint32_t v) {
        cpu_->setCpacr(v);
    };
    board_->lcd().setLogger([this](const std::string& s) {
        log(QString::fromStdString(s));
    });

    board_->attach();

    // Create the engine (open + CPU model + memory hooks) on THIS thread.
    // unicorn's uc_hook_add()/uc_ctl() crash on any worker thread, so the
    // entire simulation stays single-threaded on the object's thread.
    engineReady_ = cpu_->start(this);
    if (engineReady_) {
        auto map = [&](uint64_t base, size_t size, void* backing) {
            engineReady_ = cpu_->mapMemory(base, size, UC_PROT_ALL, backing) &&
                           engineReady_;
        };
        // real memory backed by the SoC buffers
        map(Stm32G431::kFlashBase, Stm32G431::kFlashSize, soc_->flash());
        map(Stm32G431::kSramBase, Stm32G431::kSramSize, soc_->sram());
        map(Stm32G431::kCcmBase, Stm32G431::kCcmSize, soc_->ccm());
        // dummy-backed MMIO regions (intercepted by memory hooks)
        map(0x40000000ull, 0x10000, nullptr);  // APB1
        map(0x40010000ull, 0x10000, nullptr);  // APB2
        map(0x40020000ull, 0x4000, nullptr);   // AHB1 (RCC, FLASH, DMA)
        map(0x48000000ull, 0x2000, nullptr);   // AHB2 (GPIO)
        map(0x50000000ull, 0x1000, nullptr);   // ADC
        map(0xE0000000ull, 0x1000, nullptr);   // ITM
        map(0xE0001000ull, 0x1000, nullptr);   // DWT
        map(0xE000E000ull, 0x1000, nullptr);   // PPB (SysTick/NVIC/SCB)
        map(0xE0040000ull, 0x1000, nullptr);   // TPIU

        // Probe the engine's exact single-step support ONCE (stage 7-2A
        // capability reporting). The translation cache is still empty here, so
        // the two mode switches cost nothing.
        exactStepSupported_ = cpu_->setExactSingleStep(true);
        cpu_->setExactSingleStep(false);
    }

    tickTimer_ = new QTimer(this);
    tickTimer_->setTimerType(Qt::PreciseTimer);
    tickTimer_->setInterval(2);  // 2 ms tick; batches are tiny & non-blocking
    connect(tickTimer_, &QTimer::timeout, this, &Simulator::runTick);
    wall_.start();
}

Simulator::~Simulator() {
    pause();
}

// ---------------------------------------------------------------------------
// any-thread API
// ---------------------------------------------------------------------------
void Simulator::startRun() {
    if (running_.load() || !firmwareLoaded_.load()) return;
    // --wait-debugger: the CLI asked the simulator to wait for a debugger
    // session before letting the target run (stage 7-2A).
    if (waitForDebugger_.load() && !debuggerAttached_.load()) {
        log("[ipc] run suppressed: waiting for a debugger (--wait-debugger)");
        return;
    }
    // Classic continue-from-a-breakpoint: when the target sits exactly on the
    // breakpoint it stopped at, that instruction must execute once -- otherwise
    // the first batch would re-hit it and Run / push could never advance.
    if (lastStop_.reason == StopReason::Breakpoint &&
        (cpu_->getReg(CpuRegId::PC) & ~1u) == (lastStop_.pc & ~1u)) {
        debugSkipCurrentBreakpointOnce();
    }
    running_.store(true);
    cpuFaulted_ = false;
    lastStop_ = StopInfo{};  // no stop reason while running
    wall_.restart();
    // Pacing reference for the 1:1 loop: virtual time SINCE this run started.
    // Comparing the ABSOLUTE cycle counter against the wall clock broke every
    // run after the first one -- soc_->reset() keeps cycles_ (only the CPU is
    // reset), so after a Reset or a firmware change virtualMs was already huge,
    // runTick() broke out instantly and Run did nothing at all.
    runStartCycles_ = soc_->totalCycles();
    lastSnapMs_ = 0;
    // timeline markers in the session log (see writeLogFile); the simulation is
    // always 1:1 with wall time -- there is no speed setting any more
    log("[sim] run (real time 1:1)");
    tickTimer_->start();
    emit runningChanged(true);
}

void Simulator::pause() {
    if (!running_.load()) return;
    pauseImpl();
    // A GUI Pause / debugger halt is a user halt; internal stop causes
    // (breakpoint hit, fault, reset) set their own reason right after calling
    // pauseImpl() -- using pause() there would publish a spurious UserHalt
    // stop event before the real one.
    setStop(StopReason::UserHalt, cpu_->getReg(CpuRegId::PC));
}

// Stops the run loop WITHOUT recording a stop reason (the caller sets the real
// reason immediately afterwards, so exactly ONE stop event is published).
void Simulator::pauseImpl() {
    if (!running_.load()) return;
    running_.store(false);
    tickTimer_->stop();
    log("[sim] paused");
    emit runningChanged(false);
    emit stateChanged(makeSnapshot());
}

void Simulator::setButton(int index, bool pressed) {
    board_->setButton(index, pressed);
    log(QString("[gui] B%1 %2").arg(index + 1).arg(pressed ? "pressed" : "released"));
}

void Simulator::setPulseEnabled(bool enabled) {
    board_->setPulseEnabled(enabled);
    log(QString("[gui] PA15 generator %1 (%2 Hz)")
            .arg(enabled ? "ON" : "off")
            .arg(pulseHz_, 0, 'f', 0));
}

void Simulator::setPulseFrequency(double hz) {
    pulseHz_ = hz;
    board_->setPulseFrequency(hz);
}

void Simulator::setPb4PulseEnabled(bool enabled) {
    board_->setPb4PulseEnabled(enabled);
    log(QString("[gui] PB4 generator %1 (%2 Hz)")
            .arg(enabled ? "ON" : "off")
            .arg(pb4Hz_, 0, 'f', 0));
}

void Simulator::setPb4PulseFrequency(double hz) {
    pb4Hz_ = hz;
    board_->setPb4PulseFrequency(hz);
}

void Simulator::sendUsart1Bytes(const std::vector<uint8_t>& bytes) {
    board_->serialSendToMcu(bytes);
    QString text;
    for (uint8_t b : bytes)
        text += (b >= 0x20 && b < 0x7F) ? QChar(b) : QChar('.');
    if (text.size() > 40) text = text.left(40) + "...";
    log(QString("[gui] terminal -> MCU: %1 byte(s) \"%2\"")
            .arg(bytes.size())
            .arg(text));
}

// ---------------------------------------------------------------------------
// engine-thread slots
// ---------------------------------------------------------------------------
bool Simulator::loadFirmware(const QString& path) {
    // Opening a firmware while the simulation runs is a normal user action
    // (Open -> Run -> Open another hex). Automatically stop the current run
    // instead of silently refusing the load.
    if (running_.load()) {
        pauseImpl();  // doReset() below publishes the real reason (Reset)
        log("[sim] auto-paused to load a new firmware");
    }
    FirmwareImage img;
    std::string err;
    if (!HexLoader::loadFile(path.toStdString(), img, err)) {
        emit firmwareLoadFailed(QString::fromStdString(err));
        return false;
    }
    // Build the full 128 KiB image and commit it through the SAME
    // replace/reset path the debugger programming path uses
    // (SimulatorFlashProgrammer::programEnd -> replaceFlashImage).
    std::vector<uint8_t> image(Stm32G431::kFlashSize);
    if (!Stm32G431::buildFlashImage(img, image.data(), image.size(), err)) {
        emit firmwareLoadFailed(QString::fromStdString(err));
        return false;
    }
    if (!replaceFlashImage(image)) {
        emit firmwareLoadFailed("flash commit failed");
        return false;
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "loaded %zu byte(s) from '%s'",
                  img.highestEndAddress() - img.lowestAddress(),
                  QFileInfo(path).fileName().toUtf8().constData());
    log(QString::fromUtf8(buf));
    emit firmwareLoadedSig(QString::fromUtf8(buf));
    return true;
}

// ---------------------------------------------------------------------------
// Virtual flash programming support (stage 7-2A)
//
// There is exactly ONE live flash backing store (Stm32G431::flash_), which is
// also the unicorn memory backing -- the 0x00000000 alias is derived from it.
// A debugger transaction works on a staging copy and calls replaceFlashImage()
// once at commit time; the firmware loader (loadFirmware) uses the same path,
// so "replace flash -> drop translated code -> reset" exists exactly once.
// ---------------------------------------------------------------------------
bool Simulator::readFlashImage(std::vector<uint8_t>& out) {
    out.assign(Stm32G431::kFlashSize, 0xFF);
    // through the CPU-visible path (flash base; the alias is the same bytes)
    return soc_->guestRead(Stm32G431::kFlashBase, out.data(), out.size()) ==
           DebugStatus::Ok;
}

bool Simulator::replaceFlashImage(const std::vector<uint8_t>& image) {
    if (!engineReady_) return false;
    if (image.size() != Stm32G431::kFlashSize) return false;
    if (running_.load()) pauseImpl();  // never swap code underneath a live CPU
    if (!soc_->replaceFlash(image.data(), image.size())) return false;
    firmwareLoaded_.store(true);
    // doReset(): SoC/board reset + rebuildCpu() (drops the translated code of
    // the flash AND the alias window via UC_CTL_TB_REMOVE_CACHE) + vector
    // table -> MSP/PC. The target ends Halted with StopReason::Reset.
    return doReset();
}

Simulator::MemoryMapInfo Simulator::memoryMap() {
    MemoryMapInfo m{};
    m.flashBase = Stm32G431::kFlashBase;
    m.flashSize = Stm32G431::kFlashSize;
    m.aliasBase = 0x00000000u;  // boot alias, read-only mirror of flash
    m.aliasSize = Stm32G431::kFlashSize;
    m.sramBase = Stm32G431::kSramBase;
    m.sramSize = Stm32G431::kSramSize;
    m.ccmBase = Stm32G431::kCcmBase;
    m.ccmSize = Stm32G431::kCcmSize;
    return m;
}

void Simulator::slotReset() {
    if (running_.load()) return;
    doReset();
}

void Simulator::slotStep(int batches) {
    if (running_.load() || !firmwareLoaded_.load()) return;
    board_->applyInputs();

    // Run `batches` short batches through runOneBatch (the same safe path
    // used by the interactive run loop, which slices execution at the next
    // SysTick edge). A single long uc_emu_start() that crosses several
    // SysTick underflows is unreliable with the UCRT-unicorn build, so we
    // never issue a batch larger than kMaxBatchInsns here.
    for (int i = 0; i < batches; i++) {
        runOneBatch();
    }
    emit stateChanged(makeSnapshot());
}

void Simulator::toggleBreakpoint(quint32 address) {
    QMetaObject::invokeMethod(
        this,
        [this, address] {
            // one implementation for GUI and debug target (see debugAdd*)
            if (!debugRemoveBreakpoint(address)) debugAddBreakpoint(address);
        },
        Qt::QueuedConnection);
}

void Simulator::clearBreakpoints() {
    QMetaObject::invokeMethod(this, [this] { debugClearBreakpoints(); },
                              Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// Debug target support (see Simulator.h / src/debug/)
// ---------------------------------------------------------------------------
namespace {
uint32_t normAddr(uint32_t a) { return a & ~1u; }  // Thumb normalization
}  // namespace

bool Simulator::debugAddBreakpoint(uint32_t address) {
    address = normAddr(address);
    if (debugHasBreakpoint(address)) return false;
    breakpoints_.push_back(address);
    debugLogLine(QString("breakpoint add 0x%1").arg(address, 8, 16, QChar('0')));
    return true;
}

bool Simulator::debugRemoveBreakpoint(uint32_t address) {
    address = normAddr(address);
    auto it = std::find(breakpoints_.begin(), breakpoints_.end(), address);
    if (it == breakpoints_.end()) return false;
    breakpoints_.erase(it);
    debugLogLine(
        QString("breakpoint remove 0x%1").arg(address, 8, 16, QChar('0')));
    return true;
}

bool Simulator::debugHasBreakpoint(uint32_t address) const {
    address = normAddr(address);
    return std::find(breakpoints_.begin(), breakpoints_.end(), address) !=
           breakpoints_.end();
}

void Simulator::debugClearBreakpoints() {
    if (breakpoints_.empty()) return;
    breakpoints_.clear();
    debugLogLine("breakpoints cleared");
}

// Arm the one-shot skip for the current PC: the next execution of the
// breakpoint sitting exactly there is ignored (continue/step over it). The
// flag is consumed by the precise-breakpoint batch (see runBreakpointBatch).
void Simulator::debugSkipCurrentBreakpointOnce() {
    skipOncePc_ = normAddr(cpu_->getReg(CpuRegId::PC));
    skipOnceValid_ = true;
}

// Exactly one guest instruction through the normal batch pipeline. The
// instruction executes for real inside unicorn (MMIO side effects included);
// nothing here touches PC or emulates an opcode.
bool Simulator::debugStepOne() {
    if (running_.load() || !firmwareLoaded_.load()) return false;
    const uint32_t pcBefore = normAddr(cpu_->getReg(CpuRegId::PC));
    // step() executes the instruction AT the current PC unconditionally, so a
    // breakpoint sitting there cannot stop it (classic step-over semantics);
    // runOneInstructionBatch() does not run the breakpoint check.
    runOneInstructionBatch();
    const uint32_t pcAfter = normAddr(cpu_->getReg(CpuRegId::PC));
    if (lastBatchReason_ == CpuStopReason::Fault) {
        // executeBatch() already recorded Fault
    } else {
        cpuFaulted_ = false;
        setStop(StopReason::SingleStep, pcAfter);
    }
    debugLogLine(QString("step 0x%1 -> 0x%2")
                     .arg(pcBefore, 8, 16, QChar('0'))
                     .arg(pcAfter, 8, 16, QChar('0')));
    emit stateChanged(makeSnapshot());
    return true;
}

// Reset through the existing SoC/board/CPU reset path; the target ends Halted
// with StopReason::Reset and SP/PC loaded from the vector table.
bool Simulator::debugResetHalt() {
    if (running_.load()) pauseImpl();  // never reset underneath a live CPU
    if (!doReset()) return false;
    debugLogLine(QString("reset-halt pc=0x%1")
                     .arg(normAddr(cpu_->getReg(CpuRegId::PC)), 8, 16,
                          QChar('0')));
    emit stateChanged(makeSnapshot());
    return true;
}

void Simulator::setStop(StopReason reason, uint32_t pc) {
    lastStop_.reason = reason;
    lastStop_.pc = normAddr(pc);
    // Stage 7-2A: THE single place where "the target stopped" becomes an
    // observable event (TARGET_STOPPED / TARGET_FAULTED). Runs on the owner
    // thread; the observer may only queue the event (the Debug IPC server does
    // exactly that) -- it must never call back into the target.
    if (stopObserver_ && reason != StopReason::None)
        stopObserver_(lastStop_, soc_ ? soc_->totalCycles() : 0);
}

// ---------------------------------------------------------------------------
// main execution loop
//
// The simulation runs at EXACTLY 1:1 with wall time -- a firmware delay(1000)
// takes one real second, always. There is no speed option: virtual time is
// never allowed to run ahead of the wall clock, and each tick is capped so the
// GUI thread is never blocked for more than a couple of milliseconds (if the
// host cannot keep up with a dense workload the virtual clock just lags, it
// never skips ahead).
// ---------------------------------------------------------------------------
void Simulator::runTick() {
    if (!running_.load()) {
        tickTimer_->stop();
        return;
    }

    const qint64 tickStart = wall_.elapsed();
    if (soc_->sysclkHz() > 0) {
        const double msPerCycle = 1000.0 / double(soc_->sysclkHz());
        // Catch-up window: a firmware's LCD init/first full redraw is a pure
        // MMIO burst (several bus writes per pixel through the GPIO model) and
        // cannot run at 1x on this host, so the virtual clock falls behind by
        // 100-200 ms during boot. With only the normal 4 ms/tick allowance it
        // took seconds to repay that debt (measured: 0.75x -> 0.95x over 3 s).
        // When noticeably behind, allow a longer (still bounded) window so the
        // clock is back to exactly 1:1 within a few tens of milliseconds and
        // delay(1000) really is one second. The GUI is never blocked for more
        // than kCatchUpWindowMs.
        // Virtual time measured from the start of THIS run (see startRun).
        const double virtualMsNow = double(soc_->totalCycles() - runStartCycles_) *
                                    msPerCycle;
        const bool behind = (double(wall_.elapsed()) - virtualMsNow) >
                            double(kCatchUpThresholdMs);
        const qint64 cap = behind ? kCatchUpWindowMs : 2 * kTickWallBudgetMs;
        int batches = 0;
        while (running_.load() && batches++ < kMaxBatchesPerTick) {
            const double virtualMs =
                double(soc_->totalCycles() - runStartCycles_) * msPerCycle;
            if (virtualMs >= double(wall_.elapsed())) break;  // 1:1, never ahead
            runOneBatch();
            if ((batches & 7) == 0 && wall_.elapsed() - tickStart >= cap) break;
        }
    }

    // throttled snapshot (~30 Hz)
    const qint64 now = wall_.elapsed();
    if (now - lastSnapMs_ >= 33) {
        lastSnapMs_ = now;
        emit stateChanged(makeSnapshot());
    }

    perfReportTick(now - tickStart);
}

// ---------------------------------------------------------------------------
// Session log file. Every log line (plus the [perf] diagnostics and a 5 s
// heartbeat) is appended to <exe dir>/bluesim.log and flushed line by line, so
// the data is on disk even if the GUI is frozen solid and gets killed. This is
// the channel to use when the log panel cannot be reached (it lives on another
// tab, and a stuck UI cannot be clicked).
// ---------------------------------------------------------------------------
void Simulator::writeLogFile(const QString& msg) {
    if (!logFile_.isOpen()) {
        logFile_.setFileName(QCoreApplication::applicationDirPath() +
                             "/bluesim.log");
        if (!logFile_.open(QIODevice::WriteOnly | QIODevice::Append)) return;
        logFile_.write(QString("\n=== session %1 ===\n")
                           .arg(QDateTime::currentDateTime().toString(
                               "yyyy-MM-dd HH:mm:ss"))
                           .toUtf8());
    }
    logFile_.write(msg.toUtf8());
    logFile_.write("\n");
    logFile_.flush();
}

// ---------------------------------------------------------------------------
// performance diagnostics (see Simulator.h)
//
// One [perf] line per second, emitted ONLY when the simulation is not keeping
// up (so normal runs stay quiet), plus an immediate line from runOneBatch() for
// a single batch that blocked for >= 300 ms. The line answers "hung or slow?"
// and "where does the time go?":
//   virt   virtual ms per wall ms (< 1 = slower than real time)
//   ticks  GUI ticks/s and the worst tick (how long the UI thread was blocked)
//   batch  batches/s, per tick, and the average batch length in cycles
//   MMIO   peripheral accesses/s -- a tight poll loop shows up as tens of
//          millions here (each access crosses a hook: ~1 us)
//   worst  the single slowest batch of the window split into its four phases
// ---------------------------------------------------------------------------
void Simulator::perfReportTick(qint64 tickMs) {
    ++perfTicks_;
    perfTickMaxMs_ = std::max(perfTickMaxMs_, tickMs);
    const qint64 now = wall_.elapsed();
    if (perfWindowMs_ == 0) perfWindowMs_ = now;  // first call: start the window
    const qint64 windowMs = now - perfWindowMs_;
    if (windowMs < 1000) return;

    const uint64_t cycles = soc_->totalCycles();
    const uint64_t dCycles = cycles - perfCyclesWindow_;
    const uint64_t dMmio = perfMmio_ - perfMmioWindow_;
    const uint32_t hclk = soc_->sysclkHz() ? soc_->sysclkHz() : 1u;
    const double virtMs = double(dCycles) / (double(hclk) / 1000.0);
    realTimeX_ = virtMs / double(windowMs);
    const double batchesPerTick =
        perfTicks_ ? double(perfBatches_) / double(perfTicks_) : 0.0;
    const double avgBatch =
        perfBatches_ ? double(dCycles) / double(perfBatches_) : 0.0;
    const double mmioPerSec = double(dMmio) * 1000.0 / double(windowMs);
    // exception traffic: a firmware that faults / returns from exceptions at a
    // huge rate is what makes the batches end early (avg batch << 40000)
    const uint64_t dExc = exceptionReturns_ - perfExcWindow_;
    const double excPerSec = double(dExc) * 1000.0 / double(windowMs);

    // report only when something is off (slower than 0.9x, a tick that blocked
    // the GUI, or an MMIO poll storm); otherwise only a 5 s heartbeat goes to
    // the log FILE (never to the GUI panel, which would be noise)
    const bool notable = realTimeX_ < 0.9 || perfTickMaxMs_ > 50 ||
                         mmioPerSec > 2000000.0;
    if (notable || now - lastPerfFileMs_ >= 5000) {
        const QString line =
            QString("[perf] virt %1x | %2 ticks/s (max %3 ms) | %4 batches/s "
                    "(%5/tick, avg %6 cyc) | MMIO %7M/s | exc %8k/s | "
                    "tightest slice %9 cyc @%10 | worst batch %11 ms: in %12 "
                    "/ sched %13 / cpu %14 / adv %15 (MMIO %16)")
                .arg(realTimeX_, 0, 'f', 2)
                .arg(perfTicks_)
                .arg(perfTickMaxMs_)
                .arg(perfBatches_)
                .arg(batchesPerTick, 0, 'f', 1)
                .arg(avgBatch, 0, 'f', 0)
                .arg(mmioPerSec / 1.0e6, 0, 'f', 2)
                .arg(excPerSec / 1000.0, 0, 'f', 1)
                .arg((unsigned long long)(perfMinSlice_ == ~0ull
                                              ? 0
                                              : perfMinSlice_))
                .arg(QString::fromLatin1(perfMinSliceName_))
                .arg(double(perfWorstUs_) / 1000.0, 0, 'f', 1)
                .arg(double(perfWorstPhaseUs_[0]) / 1000.0, 0, 'f', 1)
                .arg(double(perfWorstPhaseUs_[1]) / 1000.0, 0, 'f', 1)
                .arg(double(perfWorstPhaseUs_[2]) / 1000.0, 0, 'f', 1)
                .arg(double(perfWorstPhaseUs_[3]) / 1000.0, 0, 'f', 1)
                .arg((unsigned long long)perfWorstMmio_);
        if (notable) {
            lastPerfFileMs_ = now;
            log(line);
        } else {
            lastPerfFileMs_ = now;
            writeLogFile(line);  // file-only heartbeat
        }
    }

    // start the next window
    perfWindowMs_ = now;
    perfCyclesWindow_ = cycles;
    perfMmioWindow_ = perfMmio_;
    perfTicks_ = 0;
    perfBatches_ = 0;
    perfTickMaxMs_ = 0;
    perfWorstUs_ = 0;
    for (int i = 0; i < 4; i++) perfWorstPhaseUs_[i] = 0;
    perfWorstMmio_ = 0;
    perfMinSlice_ = ~0ull;
    perfMinSliceName_ = "none";
    perfExcWindow_ = exceptionReturns_;
}

void Simulator::runOneBatch() {
    const PerfClock::time_point t0 = PerfClock::now();
    const uint64_t mmio0 = perfMmio_;
    board_->applyInputs();

    // budget: never run past the next schedulable peripheral event. The board
    // is a source as well: the external signal generator's next edge must be
    // reached exactly, otherwise a capture would latch a late counter value.
    const PerfClock::time_point t1 = PerfClock::now();
    uint64_t budget = kMaxBatchInsns;
    const Stm32G431::EventSource socEv = soc_->nextEventSource();
    const Ct117eM4::SignalSource brdEv = board_->nextSignalEventSource();
    const bool socWins = socEv.cycles <= brdEv.cycles;
    const uint64_t toEvent = socWins ? socEv.cycles : brdEv.cycles;
    const char* const sliceName = socWins ? socEv.name : brdEv.name;
    if (toEvent < budget) budget = std::max<uint64_t>(toEvent, kMinBatchInsns);
    // diagnostics: remember the tightest MEANINGFUL slice of the report window
    // and who caused it (the transient 0 of a pending generator reconfiguration
    // is ignored -- it only costs one batch)
    if (toEvent >= kMinBatchInsns && toEvent < perfMinSlice_) {
        perfMinSlice_ = toEvent;
        perfMinSliceName_ = sliceName;
    }
    const PerfClock::time_point t2 = PerfClock::now();
    if (!breakpoints_.empty()) {
        // stage 7-1 plan B: precise breakpoints (one instruction at a time)
        runBreakpointBatch(uint32_t(budget), t0, t1, t2, mmio0);
        return;
    }
    // Untouched fast path: multi-instruction blocks, no chaining restrictions.
    setExactStepMode(false);
    executeBatch(uint32_t(budget), t0, t1, t2, mmio0);
}

// ---------------------------------------------------------------------------
// Precise breakpoints (stage 7-1 plan B)
//
// Why not UC_HOOK_CODE + uc_emu_stop (plan A)? Measured on this unicorn 2.1.4
// build: inside a hot, chained translation block the stop request is only
// observed when the chain breaks, so a 3-instruction loop overshot by 512
// iterations before the emulation actually stopped -- the target instruction
// had executed hundreds of times although the debugger must stop BEFORE it.
// (tests/debug_target_selftest.cpp "stress 10000x" pins the required
// semantics; see PROJECT_HANDOFF 7.2c.)
//
// Plan B keeps full correctness by checking the breakpoint set on the HOST
// before each guest instruction and executing exactly one instruction per
// iteration. It is slow (a full batch pipeline per instruction), but only the
// debug-with-breakpoints mode pays for it: with no breakpoint set, runOneBatch
// takes the untouched fast path above, and the run loop's wall-clock budget
// still returns control to Qt regularly.
// ---------------------------------------------------------------------------
void Simulator::runBreakpointBatch(uint32_t budget, PerfClock::time_point t0,
                                   PerfClock::time_point t1,
                                   PerfClock::time_point t2,
                                   uint64_t mmioBefore) {
    (void)t0;
    (void)t1;
    (void)t2;
    (void)mmioBefore;
    // Every executeBatch(1) below must execute EXACTLY one instruction for the
    // host-side breakpoint check to be precise.
    setExactStepMode(true);
    const uint32_t maxInsns = std::min<uint32_t>(budget, kPreciseBatchInsns);
    for (uint32_t i = 0; i < maxInsns; ++i) {
        const uint32_t pcNow = normAddr(cpu_->getReg(CpuRegId::PC));
        if (debugHasBreakpoint(pcNow)) {
            if (skipOnceValid_ && skipOncePc_ == pcNow) {
                skipOnceValid_ = false;  // continue/step over this one
            } else {
                // stop BEFORE the instruction: PC is the breakpoint address and
                // the instruction has not executed
                skipOnceValid_ = false;  // a stale skip must never linger
                pauseImpl();  // no-op when already halted (test/debug drives)
                setStop(StopReason::Breakpoint, pcNow);
                debugLogLine(QString("breakpoint hit 0x%1")
                                 .arg(pcNow, 8, 16, QChar('0')));
                emit breakpointHit(pcNow);
                return;
            }
        }
        const PerfClock::time_point now = PerfClock::now();
        if (!executeBatch(1, now, now, now, perfMmio_)) return;
    }
    // the skip must not leak into the next batch if its PC was never reached
    skipOnceValid_ = false;
}

// One guest instruction through the *identical* pipeline (debug single step).
// Only the instruction budget differs -- there is no separate "debug
// execution" path: MMIO effects, virtual clock advance, exception handling and
// interrupt servicing behave exactly like a normal batch.
void Simulator::runOneInstructionBatch() {
    const PerfClock::time_point t0 = PerfClock::now();
    const uint64_t mmio0 = perfMmio_;
    board_->applyInputs();
    const PerfClock::time_point t1 = PerfClock::now();
    const PerfClock::time_point t2 = PerfClock::now();
    // exact single-step: the batch below is guaranteed to execute exactly one
    // guest instruction (see UC_CTL_EXACT_SINGLE_STEP in unicorn-2.1.4)
    setExactStepMode(true);
    executeBatch(1, t0, t1, t2, mmio0);
}

// Toggle the engine's exact single-step mode (one guest instruction per
// translated block). Called on the debugger paths only; the plain run path
// disables it again. UnicornCpu caches the state, so the translation cache is
// flushed only when the mode really changes. If the loaded unicorn.dll does
// not implement UC_CTL_EXACT_SINGLE_STEP (older build) the mode stays off and
// a single warning is logged -- stepping then falls back to the legacy
// (inexact) count-based stop.
void Simulator::setExactStepMode(bool enable) {
    if (exactStepMode_ == enable) return;
    if (!cpu_->setExactSingleStep(enable)) {
        if (!exactStepWarned_) {
            exactStepWarned_ = true;
            log("exact single-step mode NOT available in the loaded "
                "unicorn.dll (missing UC_CTL_EXACT_SINGLE_STEP); precise "
                "stepping falls back to the legacy count-based stop");
        }
        return;
    }
    exactStepMode_ = enable;
    if (enable)
        debugLogLine("exact single-step mode on (one instruction per block)");
    else
        debugLogLine("exact single-step mode off (fast run path)");
}

bool Simulator::executeBatch(uint32_t budget, PerfClock::time_point t0,
                             PerfClock::time_point t1, PerfClock::time_point t2,
                             uint64_t mmioBefore) {
    const PerfClock::time_point t3 = PerfClock::now();
    const uint32_t pc = cpu_->getReg(CpuRegId::PC) & ~1u;
    const CpuStopReason reason = cpu_->runBatch(pc, budget);
    lastBatchReason_ = reason;

    const PerfClock::time_point t4 = PerfClock::now();
    const uint32_t executed = cpu_->executedLastBatch();
    executedInsns_ += executed;
    soc_->advanceTime(executed);
    // Board signal sources: the generator produces its edges in virtual time
    // and pushes them into the pin (with their exact cycle) right after the
    // peripheral world has been advanced to the same instant.
    board_->advanceSignals();
    const PerfClock::time_point t5 = PerfClock::now();

    // diagnostics: keep the worst batch of the report window (see perfReportTick)
    ++perfBatches_;
    const qint64 phaseUs[4] = {usBetween(t0, t1), usBetween(t1, t2),
                               usBetween(t3, t4), usBetween(t4, t5)};
    const qint64 totalUs = usBetween(t0, t5);
    if (totalUs > perfWorstUs_) {
        perfWorstUs_ = totalUs;
        for (int i = 0; i < 4; i++) perfWorstPhaseUs_[i] = phaseUs[i];
        perfWorstMmio_ = perfMmio_ - mmioBefore;
    }
    if (totalUs >= kSlowBatchWarnUs)
        log(QString("[perf] slow batch: %1 ms (inputs %2 / sched %3 / CPU %4 / "
                    "advance %5 ms, MMIO %6, budget %7, exec %8)")
                .arg(double(totalUs) / 1000.0, 0, 'f', 1)
                .arg(double(phaseUs[0]) / 1000.0, 0, 'f', 1)
                .arg(double(phaseUs[1]) / 1000.0, 0, 'f', 1)
                .arg(double(phaseUs[2]) / 1000.0, 0, 'f', 1)
                .arg(double(phaseUs[3]) / 1000.0, 0, 'f', 1)
                .arg((unsigned long long)perfWorstMmio_)
                .arg((unsigned)budget)
                .arg(executed));

    switch (reason) {
    case CpuStopReason::ExcReturn:
        ++exceptionReturns_;
        doExceptionReturn(cpu_->lastExcReturn());
        break;
    case CpuStopReason::Fault: {
        const uint32_t faultPc = cpu_->getReg(CpuRegId::PC) & ~1u;
        cpuFaulted_ = true;
        pauseImpl();  // the reason below is the one and only stop event
        setStop(StopReason::Fault, faultPc);
        const QString err = QString::fromStdString(cpu_->lastError());
        log(err);
        emit faulted(err);
        return false;
    }
    default:
        break;
    }

    if (soc_->consumeResetRequest()) {
        log("system reset requested by firmware (AIRCR.SYSRESETREQ)");
        pause();
        doReset();
        return false;
    }

    afterBatchSync();
    serviceInterrupts();
    return true;
}

void Simulator::serviceInterrupts() {
    int exc = selectPendingException();
    if (exc != 0) doExceptionEntry(exc);
}

// ---------------------------------------------------------------------------
// Cortex-M exception injection (manual NVIC)
// ---------------------------------------------------------------------------
int Simulator::selectPendingException() {
    const NvicController& nvic = soc_->ppb.nvic;
    const uint32_t primask = cpu_->getReg(CpuRegId::PRIMASK);
    const uint32_t basepri = cpu_->getReg(CpuRegId::BASEPRI);

    int best = 0;
    uint8_t bestPrio = 0xFF;

    auto consider = [&](int exc, bool enabled, uint8_t prioByte) {
        if (!enabled) return;
        if (primask != 0) return;  // global mask
        if (basepri != 0 && prioByte >= basepri) return;
        // nesting: an equal or lower priority exception cannot preempt
        if (activeException_ != 0 && prioByte >= activePriorityByte_)
            return;
        if (prioByte < bestPrio) {
            bestPrio = prioByte;
            best = exc;
        }
    };

    // system exceptions: 15 = SysTick (enabled by TICKINT), 14 = PendSV
    consider(15, nvic.isPending(15) &&
                     (soc_->ppb.systick.csr() & SysTickTimer::TICKINT),
             nvic.priorityByte(15));
    consider(14, nvic.isPending(14), nvic.priorityByte(14));

    // external IRQs (pending AND enabled)
    for (int irq = 0; irq < NvicController::kMaxExternalIrq; irq++) {
        if (nvic.isPending(irq + 16) && nvic.irqEnabled(irq))
            consider(irq + 16, true, nvic.priorityByte(irq + 16));
    }
    return best;
}

void Simulator::doExceptionEntry(int exc) {
    NvicController& nvic = soc_->ppb.nvic;

    const uint32_t control = cpu_->getReg(CpuRegId::CONTROL);
    const bool usePsp = (control & 2u) != 0;
    const CpuRegId spReg = usePsp ? CpuRegId::PSP : CpuRegId::MSP;

    uint32_t sp = cpu_->getReg(spReg);
    uint32_t frame[8];
    frame[0] = cpu_->getReg(CpuRegId::R0);
    frame[1] = cpu_->getReg(CpuRegId::R1);
    frame[2] = cpu_->getReg(CpuRegId::R2);
    frame[3] = cpu_->getReg(CpuRegId::R3);
    frame[4] = cpu_->getReg(CpuRegId::R12);
    frame[5] = cpu_->getReg(CpuRegId::LR);
    frame[6] = cpu_->getReg(CpuRegId::PC);
    frame[7] = cpu_->getReg(CpuRegId::XPSR) | 0x01000000u;  // keep T bit set

    sp -= 32;
    if (!cpu_->writeGuest(sp, frame, sizeof(frame))) {
        log(QString("exception %1: stack push failed").arg(exc));
        pause();
        return;
    }
    cpu_->setReg(spReg, sp);
    if (cpu_->getReg(CpuRegId::SP) != sp) cpu_->setReg(CpuRegId::SP, sp);
    cpu_->setReg(CpuRegId::LR, usePsp ? 0xFFFFFFFDu : 0xFFFFFFF9u);

    uint32_t handler = 0;
    if (!soc_->readWord(soc_->ppb.vtor() + 4u * uint32_t(exc), handler) ||
        handler == 0xFFFFFFFFu) {
        log(QString("exception %1: vector not programmed").arg(exc));
        pause();
        emit faulted(QString("exception %1 has no vector").arg(exc));
        return;
    }

    // Enter Handler mode so QEMU treats the PC differently for this ISR and
    // the core's IPSR reads the exception number (architecturally real).
    // v7m.exception drives Handler-mode semantics; it is not writable via the
    // read-only IPSR register, so route through the patched engine export.
    cpu_->setV7mException(exc);
    cpu_->setReg(CpuRegId::IPSR, uint32_t(exc));
    cpu_->setReg(CpuRegId::PC, handler & ~1u);
    nvic.clearPend(exc);
    activeExceptionStack_.push_back(exc);
    activeException_ = exc;
    activePriorityByte_ = nvic.priorityByte(exc);
    nvic.setActiveException(exc);
}

// Reconcile our manual exception bookkeeping with the core's IPSR after a
// batch. When QEMU natively performs an exception return (handler `bx lr`),
// it clears IPSR without going through our ExcReturn path.
void Simulator::afterBatchSync() {
    const bool wasInHandler = (activeException_ != 0);
    const uint32_t ipsr = cpu_->getReg(CpuRegId::IPSR);
    bool inHandler = (ipsr != 0);
    if (inHandler && ipsr < 16) {
        activeException_ = int(ipsr);
    } else {
        activeException_ = int(ipsr);
    }
    activeExceptionStack_.clear();
    if (inHandler) activeExceptionStack_.push_back(activeException_);
    activePriorityByte_ = inHandler ? soc_->ppb.nvic.priorityByte(activeException_) : 0;
    soc_->ppb.nvic.setActiveException(activeException_);
    if (wasInHandler && !inHandler) ++exceptionReturns_;
}

void Simulator::doExceptionReturn(uint32_t excReturn) {
    if (activeExceptionStack_.empty()) {
        log("EXC_RETURN in thread mode - ignoring");
        // PC currently holds the magic value; leave it, firmware is broken
        return;
    }
    const bool usePsp = (excReturn & 4u) != 0;
    const CpuRegId spReg = usePsp ? CpuRegId::PSP : CpuRegId::MSP;

    uint32_t sp = cpu_->getReg(spReg);
    uint32_t frame[8];
    if (!cpu_->readGuest(sp, frame, sizeof(frame))) {
        log("exception return: stack pop failed");
        pause();
        return;
    }

    cpu_->setReg(CpuRegId::R0, frame[0]);
    cpu_->setReg(CpuRegId::R1, frame[1]);
    cpu_->setReg(CpuRegId::R2, frame[2]);
    cpu_->setReg(CpuRegId::R3, frame[3]);
    cpu_->setReg(CpuRegId::R12, frame[4]);
    cpu_->setReg(CpuRegId::LR, frame[5]);
    cpu_->setReg(CpuRegId::XPSR, frame[7] | 0x01000000u);
    cpu_->setReg(CpuRegId::PC, frame[6] & ~1u);

    sp += 32;
    cpu_->setReg(spReg, sp);
    if (cpu_->getReg(CpuRegId::SP) != sp) cpu_->setReg(CpuRegId::SP, sp);

    activeExceptionStack_.pop_back();
    activeException_ =
        activeExceptionStack_.empty() ? 0 : activeExceptionStack_.back();
    activePriorityByte_ =
        activeException_ ? soc_->ppb.nvic.priorityByte(activeException_) : 0;
    soc_->ppb.nvic.setActiveException(activeException_);
}

// ---------------------------------------------------------------------------
// reset / CPU lifecycle
// ---------------------------------------------------------------------------
void Simulator::rebuildCpu() {
    // The engine (hooks + memory map) was created once on the main thread.
    // loadFirmware rewrites the flash in place (the mapped backing buffer);
    // unicorn already drops affected translation blocks when memory is written
    // through its own API (verified experimentally), so this invalidation is
    // belt-and-braces -- it also covers a future writer that bypasses that path.
    // uc_ctl is only unsafe off the engine's own thread; both loadFirmware and
    // doReset run on the GUI thread, which is where the engine lives.
    if (!engineReady_) {
        emit faulted(QString::fromStdString(cpu_->lastError()));
        return;
    }
    cpu_->invalidateRegion(0x08000000ull, 0x08020000ull);  // flash (128 KB)
    cpu_->invalidateRegion(0x00000000ull, 0x00020000ull);  // flash alias
    cpu_->invalidateRegion(0x20000000ull, 0x20020000ull);  // SRAM (cleared)
}

bool Simulator::doReset() {
    activeExceptionStack_.clear();
    activeException_ = 0;
    activePriorityByte_ = 0;
    exceptionReturns_ = 0;
    executedInsns_ = 0;
    lastBatchReason_ = CpuStopReason::None;

    soc_->reset();
    board_->attach();
    rebuildCpu();

    // Cortex-M4 boot: SP/PC from the vector table (VTOR reset value = 0,
    // flash aliased at 0x00000000)
    uint32_t sp = 0, pc = 0;
    if (!soc_->readWord(0, sp) || !soc_->readWord(4, pc)) {
        cpuFaulted_ = true;
        setStop(StopReason::Fault, cpu_->getReg(CpuRegId::PC));
        emit faulted("no vector table in flash");
        return false;
    }
    cpu_->setReg(CpuRegId::MSP, sp);
    cpu_->setReg(CpuRegId::PSP, sp);
    cpu_->setReg(CpuRegId::SP, sp);
    cpu_->setReg(CpuRegId::PC, pc & ~1u);
    cpu_->setReg(CpuRegId::LR, 0xFFFFFFFFu);
    cpu_->setReg(CpuRegId::XPSR, 0x01000000u);
    cpu_->setReg(CpuRegId::IPSR, 0);
    cpu_->setReg(CpuRegId::CONTROL, 0);
    cpu_->setReg(CpuRegId::PRIMASK, 0);
    cpu_->setReg(CpuRegId::BASEPRI, 0);
    cpu_->setReg(CpuRegId::FAULTMASK, 0);

    cpuFaulted_ = false;
    setStop(StopReason::Reset, pc & ~1u);

    log(QString("reset: SP=0x%1 PC=0x%2")
            .arg(sp, 8, 16, QChar('0'))
            .arg(pc, 8, 16, QChar('0')));
    emit stateChanged(makeSnapshot());
    return true;
}

// ---------------------------------------------------------------------------
// ICpuHost (engine thread, from unicorn hooks)
// ---------------------------------------------------------------------------
uint32_t Simulator::cpuMmioRead(uint32_t addr, uint32_t size) {
    ++perfMmio_;
    return soc_->mmioRead(addr, size);
}

void Simulator::cpuMmioWrite(uint32_t addr, uint32_t value, uint32_t size) {
    ++perfMmio_;
    soc_->mmioWrite(addr, value, size);
}

bool Simulator::cpuFetchUnmapped(uint64_t addr) {
    return (addr & 0xFF000000ull) == 0xFF000000ull;  // EXC_RETURN magic
}

void Simulator::cpuIntrEvent(int intno) {
    log(QString("CPU exception event (%1), pausing").arg(intno));
}

// ---------------------------------------------------------------------------
SimSnapshot Simulator::makeSnapshot() const {
    SimSnapshot s;
    s.firmwareLoaded = firmwareLoaded_.load();
    s.running = running_.load();
    s.pc = cpu_->getReg(CpuRegId::PC);
    s.sp = cpu_->getReg(CpuRegId::SP);
    s.lr = cpu_->getReg(CpuRegId::LR);
    s.xpsr = cpu_->getReg(CpuRegId::XPSR);
    s.ipsr = cpu_->getReg(CpuRegId::IPSR);
    s.control = cpu_->getReg(CpuRegId::CONTROL);
    s.primask = cpu_->getReg(CpuRegId::PRIMASK);
    s.basepri = cpu_->getReg(CpuRegId::BASEPRI);
    for (int i = 0; i < 13; i++)
        s.regs[i] = cpu_->getReg(CpuRegId(int(i)));
    s.cycles = soc_->totalCycles();
    s.sysclkHz = soc_->sysclkHz();
    for (int i = 0; i < 8; i++) s.leds[i] = board_->led(i);
    s.buttons = 0;
    for (int i = 0; i < 4; i++)
        if (board_->button(i)) s.buttons |= uint8_t(1u << i);
    s.systickCsr = soc_->ppb.systick.csr();
    s.systickRvr = soc_->ppb.systick.rvr();
    s.systickCvr = soc_->ppb.systick.cvr();
    s.gpioC_Odr = soc_->gpioC.odr();
    s.gpioC_Idr = soc_->gpioC.pinLevels();
    s.gpioB_Idr = soc_->gpioB.pinLevels();
    s.gpioA_Idr = soc_->gpioA.pinLevels();
    s.rccCr = soc_->rcc.debugReadReg(0x00);
    s.rccCfgr = soc_->rcc.debugReadReg(0x08);
    s.lcdDirty = board_->lcd().dirty();
    s.lcdCommandWrites = board_->lcd().commandWriteCount();
    s.lcdPixelWrites = board_->lcd().pixelWriteCount();
    s.lcdLastCommand = board_->lcd().lastCommand();
    // Board-level signal view: PA7 waveform as measured at the pin, the external
    // pulse source settings and the potentiometer positions (the GUI never
    // sees a TIM or ADC register).
    const DigitalSignalMonitor::Stats pa7 = board_->pa7Waveform();
    s.pa7Active = pa7.active;
    s.pa7Level = pa7.level;
    s.pa7Frequency = pa7.frequencyHz;
    s.pa7Duty = pa7.duty;
    s.pa7Edges = pa7.edges;
    const PulseInputSource& pulse = board_->pulseSource();
    s.pulseEnabled = pulse.enabled();
    s.pulseLevel = pulse.level();
    s.pulseFrequency = pulse.frequency();
    const PulseInputSource& pb4 = board_->pb4PulseSource();
    s.pb4PulseEnabled = pb4.enabled();
    s.pb4PulseLevel = pb4.level();
    s.pb4PulseFrequency = pb4.frequency();
    s.r37Voltage = board_->r37Voltage();
    s.r38Voltage = board_->r38Voltage();
    // serial terminal / USART1: everything comes from the USART model and the
    // board's peer -- the GUI only displays it
    const Usart& u1 = soc_->usart1;
    s.usart1ClockEnabled = u1.clockEnabled();
    s.usart1Enabled = u1.enabled();
    s.usart1TxEnabled = u1.txEnabled();
    s.usart1RxEnabled = u1.rxEnabled();
    s.usart1Baud = u1.baudRate();
    s.usart1WordBits = u1.wordBits();
    s.usart1Parity = u1.parity();
    s.usart1StopBitsHalf = u1.stopBitsHalf();
    s.usart1Rxne = u1.rxne();
    s.usart1Tc = u1.tc();
    s.usart1Ore = u1.ore();
    const VirtualSerialPeer& peer = board_->serialPeer();
    s.usart1TxPending = uint32_t(peer.pendingFromMcu());
    s.usart1TxTotal = peer.bytesToPcTotal();
    s.usart1RxTotal = peer.bytesFromPcTotal();
    s.usart1Queued = uint32_t(peer.pendingToMcu());
    s.realTimeX = realTimeX_;
    s.activeException = activeException_;
    s.exceptionReturns = exceptionReturns_;
    return s;
}
