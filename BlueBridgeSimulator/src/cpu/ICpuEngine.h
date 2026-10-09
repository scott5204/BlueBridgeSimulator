#pragma once

#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// CPU emulator abstraction.
//
// The simulator drives the engine in instruction-count batches; between
// batches it services the virtual clock (SysTick / timers) and injects
// Cortex-M exceptions. MMIO accesses are delegated to the host (SoC model)
// through ICpuHost.
// ============================================================================

// Engine-neutral register identifiers (Cortex-M view).
enum class CpuReg : int {
    R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12,
    SP,   // current stack pointer (MSP or PSP depending on CONTROL.SPSEL)
    LR,
    PC,
    XPSR,
    IPSR,
    APSR,
    MSP,
    PSP,
    PRIMASK,
    BASEPRI,
    FAULTMASK,
    CONTROL,
    S0, S1, S2, S3, S4, S5, S6, S7,       // VFPv4 single-precision (FPU)
    S8, S9, S10, S11, S12, S13, S14, S15,
    S16, S17, S18, S19, S20, S21, S22, S23,
    S24, S25, S26, S27, S28, S29, S30, S31,
    FPSCR,
};

enum class CpuStopReason {
    None,            // not running / not started
    CountReached,    // batch instruction budget exhausted
    StopRequested,   // uc_emu_stop() from another thread
    ExcReturn,       // PC loaded an EXC_RETURN magic value
    Fault,           // unmapped fetch / invalid insn / other CPU fault
};

class ICpuHost {
public:
    virtual ~ICpuHost() = default;

    // Called before a MMIO read is performed by the CPU; must return the
    // value the CPU shall observe.
    virtual uint32_t cpuMmioRead(uint32_t addr, uint32_t size) = 0;
    // Called on a MMIO write by the CPU.
    virtual void cpuMmioWrite(uint32_t addr, uint32_t value, uint32_t size) = 0;
    // CPU tried to fetch at an unmapped address; return true if the host
    // handled it (EXC_RETURN magic) and emulation should stop gracefully.
    virtual bool cpuFetchUnmapped(uint64_t addr) = 0;
    // CPU raised an interrupt/breakpoint exception (BKPT, SVC, ...).
    virtual void cpuIntrEvent(int intno) = 0;
};

class ICpuEngine {
public:
    virtual ~ICpuEngine() = default;

    virtual bool start(ICpuHost* host) = 0;
    virtual void shutdown() = 0;

    virtual uint32_t getReg(CpuReg reg) = 0;
    virtual void setReg(CpuReg reg, uint32_t value) = 0;

    // Checked register access (debugger path): returns false when the engine
    // does not implement the register or the access failed. getReg/setReg stay
    // for the hot paths where the engine is known to support the register.
    virtual bool readRegister(CpuReg reg, uint32_t& value) = 0;
    virtual bool writeRegister(CpuReg reg, uint32_t value) = 0;

    // Write a coprocessor register (used for CPACR -> FPU enable).
    virtual bool setCpacr(uint32_t value) = 0;

    // Enter/leave ARMv7-M Handler mode on the core. Default no-op; engines
    // that can honour it (by writing v7m.exception) override.
    virtual void setV7mException(int exc) {
        (void)exc;
    }

    // Enable/disable exact single-step mode: every translated block then holds
    // at most one guest instruction, so runBatch(pc, 1) executes EXACTLY one
    // instruction (precise debugger stepping / precise breakpoints).
    // Toggling flushes the translation cache; the normal run path is
    // unaffected while disabled. Returns false when the engine (or the loaded
    // unicorn build) does not support it -- callers must not rely on exactness
    // then.
    virtual bool setExactSingleStep(bool enable) {
        (void)enable;
        return false;
    }

    virtual bool mapMemory(uint64_t base, size_t size, uint32_t perms,
                           void* backing) = 0;

    // Execute up to insnCount instructions starting at pc (Thumb).
    virtual CpuStopReason runBatch(uint32_t pc, uint32_t insnCount) = 0;
    // Number of instructions actually executed by the last runBatch().
    virtual uint32_t executedLastBatch() const = 0;
    // The EXC_RETURN magic from the last unmapped fetch (valid when
    // runBatch returned CpuStopReason::ExcReturn).
    virtual uint32_t lastExcReturn() const = 0;

    // Thread-safe: ask a running batch to stop as soon as possible.
    virtual void requestStop() = 0;

    // Host memory peek/poke (used for exception frames on the stack).
    virtual bool writeGuest(uint64_t addr, const void* data, size_t size) = 0;
    virtual bool readGuest(uint64_t addr, void* data, size_t size) = 0;

    // Drop cached translation blocks in [base, end) (after rewriting flash).
    virtual bool invalidateRegion(uint64_t base, uint64_t end) = 0;

    // NOTE (stage 7-1): execution breakpoints are NOT implemented in the
    // engine. uc_emu_stop() from inside a UC_HOOK_CODE callback was measured
    // to stop up to hundreds of instructions LATE on this unicorn build (TB
    // chaining skips the exit-request poll), which breaks the "stop exactly at
    // the breakpoint address" contract. The Simulator therefore runs precise
    // host-side breakpoints one instruction at a time (see
    // Simulator::runBreakpointBatch).

    virtual std::string lastError() const = 0;
};
