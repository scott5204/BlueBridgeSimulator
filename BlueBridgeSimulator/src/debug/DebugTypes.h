#pragma once

#include <cstdint>

// ============================================================================
// Debugger-neutral vocabulary for the Virtual Debug Target.
//
// This header is deliberately dependency-free (no Qt, no unicorn, no STM32
// types) so the same definitions are shared by
//   * the simulator core (StopInfo is the single source of truth for "why did
//     the virtual MCU stop"), and
//   * the IDebugTarget abstraction (future AGDI DLL / CLI / GDB stub frontends
//     must not need to know about unicorn or Qt).
// ============================================================================

// Overall state of the virtual target.
//   Halted  - the whole virtual board is frozen; virtual time does not run
//   Running - free-running with 1:1 real-time pacing
//   Reset   - a reset is in progress; this implementation performs the reset
//             synchronously and reports Halted + StopReason::Reset when
//             resetHalt() returns (a debugger only ever observes the target
//             after the reset has been performed)
//   Fault   - the CPU could not continue (unmapped fetch, engine error, ...)
enum class TargetState {
    Halted,
    Running,
    Reset,
    Fault,
};

// Why the target stopped (see StopInfo::reason).
enum class StopReason {
    None,         // not stopped (target is running, or nothing happened yet)
    UserHalt,     // the debugger/user asked it to stop (halt(), GUI Pause)
    Breakpoint,   // an execution breakpoint was hit
    SingleStep,   // one instruction was executed by step()
    Reset,        // the target was reset and is left halted
    Fault,        // the CPU hit an unrecoverable execution fault
};

struct StopInfo {
    StopReason reason = StopReason::None;
    uint32_t pc = 0;  // instruction address, Thumb bit cleared
};

// Explicit result type for every debug API call (no exceptions).
enum class DebugStatus {
    Ok,
    InvalidState,      // e.g. step() while the target is running
    InvalidRegister,   // no such register (or the engine rejected it)
    InvalidAddress,    // outside the guest memory map
    Unsupported,       // valid request that this stage does not implement
                       // (e.g. ordinary memory writes to flash)
    CpuError,          // the CPU engine reported an error
    // ---- virtual flash programming transaction (stage 7-2A, IFlashProgrammer)
    ProgramNotActive,     // no transaction open (or already finished)
    ProgramAlreadyActive, // programBegin() while a transaction is open
    ProgramTokenInvalid,  // wrong/stale program token
    ProgramRangeInvalid,  // erase/write outside the physical flash range
};

// Static facts about the target a frontend (AGDI DLL) must NOT hard-code.
// Everything here is read from the model at run time -- the memory map comes
// from the Simulator/SoC constants, never from the real-chip datasheet.
struct DebugTargetInfo {
    const char* targetName = "";    // e.g. "STM32G431RBT6"
    const char* boardName = "";     // e.g. "CT117E-M4"
    const char* architecture = "";  // e.g. "ARM Cortex-M4F"
    bool littleEndian = true;
    uint32_t flashBase = 0, flashSize = 0;        // physical programming range
    uint32_t flashAliasBase = 0, flashAliasSize = 0;  // read-only boot alias
    uint32_t sramBase = 0, sramSize = 0;
    uint32_t ccmBase = 0, ccmSize = 0;
    // capabilities (advertisement; see docs/debug_ipc_protocol.md)
    bool exactSingleStep = false;
    bool executionBreakpoint = false;
    bool dataWatchpoint = false;
    bool fpu = false;
    bool flashProgramming = false;
    bool asyncStopEvent = false;
};

// Debugger register identifiers (Cortex-M4F view). The translation to engine
// constants (UC_ARM_REG_*) happens inside the CPU adapter; no frontend may see
// engine-specific register ids.
enum class DebugRegister {
    R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12,
    SP,   // current stack pointer (MSP or PSP depending on CONTROL.SPSEL)
    LR,
    PC,
    XPSR,
    MSP,
    PSP,
    PRIMASK,
    BASEPRI,
    FAULTMASK,
    CONTROL,
    S0, S1, S2, S3, S4, S5, S6, S7,
    S8, S9, S10, S11, S12, S13, S14, S15,
    S16, S17, S18, S19, S20, S21, S22, S23,
    S24, S25, S26, S27, S28, S29, S30, S31,
    FPSCR,
};

namespace debug {

inline const char* toString(TargetState s) {
    switch (s) {
    case TargetState::Halted: return "Halted";
    case TargetState::Running: return "Running";
    case TargetState::Reset: return "Reset";
    case TargetState::Fault: return "Fault";
    }
    return "?";
}

inline const char* toString(StopReason r) {
    switch (r) {
    case StopReason::None: return "None";
    case StopReason::UserHalt: return "UserHalt";
    case StopReason::Breakpoint: return "Breakpoint";
    case StopReason::SingleStep: return "SingleStep";
    case StopReason::Reset: return "Reset";
    case StopReason::Fault: return "Fault";
    }
    return "?";
}

inline const char* toString(DebugStatus s) {
    switch (s) {
    case DebugStatus::Ok: return "Ok";
    case DebugStatus::InvalidState: return "InvalidState";
    case DebugStatus::InvalidRegister: return "InvalidRegister";
    case DebugStatus::InvalidAddress: return "InvalidAddress";
    case DebugStatus::Unsupported: return "Unsupported";
    case DebugStatus::CpuError: return "CpuError";
    case DebugStatus::ProgramNotActive: return "ProgramNotActive";
    case DebugStatus::ProgramAlreadyActive: return "ProgramAlreadyActive";
    case DebugStatus::ProgramTokenInvalid: return "ProgramTokenInvalid";
    case DebugStatus::ProgramRangeInvalid: return "ProgramRangeInvalid";
    }
    return "?";
}

}  // namespace debug