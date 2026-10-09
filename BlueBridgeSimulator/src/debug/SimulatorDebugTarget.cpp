#include "debug/SimulatorDebugTarget.h"

#include <cassert>

#include "cpu/ICpuEngine.h"
#include "sim/Simulator.h"

// ============================================================================
// DebugRegister -> CpuReg mapping. Lives here (the adapter), so neither the
// debug API nor a future frontend ever sees an engine register constant.
// ============================================================================
namespace {

bool toCpuReg(DebugRegister reg, CpuReg& out) {
    switch (reg) {
    case DebugRegister::R0: out = CpuReg::R0; return true;
    case DebugRegister::R1: out = CpuReg::R1; return true;
    case DebugRegister::R2: out = CpuReg::R2; return true;
    case DebugRegister::R3: out = CpuReg::R3; return true;
    case DebugRegister::R4: out = CpuReg::R4; return true;
    case DebugRegister::R5: out = CpuReg::R5; return true;
    case DebugRegister::R6: out = CpuReg::R6; return true;
    case DebugRegister::R7: out = CpuReg::R7; return true;
    case DebugRegister::R8: out = CpuReg::R8; return true;
    case DebugRegister::R9: out = CpuReg::R9; return true;
    case DebugRegister::R10: out = CpuReg::R10; return true;
    case DebugRegister::R11: out = CpuReg::R11; return true;
    case DebugRegister::R12: out = CpuReg::R12; return true;
    case DebugRegister::SP: out = CpuReg::SP; return true;
    case DebugRegister::LR: out = CpuReg::LR; return true;
    case DebugRegister::PC: out = CpuReg::PC; return true;
    case DebugRegister::XPSR: out = CpuReg::XPSR; return true;
    case DebugRegister::MSP: out = CpuReg::MSP; return true;
    case DebugRegister::PSP: out = CpuReg::PSP; return true;
    case DebugRegister::PRIMASK: out = CpuReg::PRIMASK; return true;
    case DebugRegister::BASEPRI: out = CpuReg::BASEPRI; return true;
    case DebugRegister::FAULTMASK: out = CpuReg::FAULTMASK; return true;
    case DebugRegister::CONTROL: out = CpuReg::CONTROL; return true;
    case DebugRegister::FPSCR: out = CpuReg::FPSCR; return true;
    default:
        // S0..S31 are contiguous in both enums
        if (reg >= DebugRegister::S0 && reg <= DebugRegister::S31) {
            const int off = int(reg) - int(DebugRegister::S0);
            out = static_cast<CpuReg>(int(CpuReg::S0) + off);
            return true;
        }
        return false;
    }
}

}  // namespace

SimulatorDebugTarget::SimulatorDebugTarget(Simulator& sim)
    : sim_(sim), ownerThread_(std::this_thread::get_id()) {}

void SimulatorDebugTarget::checkThread() const {
#ifndef NDEBUG
    assert(std::this_thread::get_id() == ownerThread_ &&
           "SimulatorDebugTarget: call must run on the simulator's owner "
           "thread (unicorn.dll is single-threaded)");
#endif
}

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
TargetState SimulatorDebugTarget::state() const {
    // The Simulator owns the truth; this never keeps a second copy.
    if (sim_.cpuFaulted()) return TargetState::Fault;
    return sim_.isRunning() ? TargetState::Running : TargetState::Halted;
}

StopInfo SimulatorDebugTarget::stopInfo() const { return sim_.lastStopInfo(); }

uint64_t SimulatorDebugTarget::virtualCycles() const {
    return sim_.virtualCycles();
}

bool SimulatorDebugTarget::firmwareLoaded() const {
    return sim_.firmwareLoaded();
}

// Static facts for GET_CAPABILITIES / HELLO. The memory map comes from the
// Simulator (i.e. the SoC constants) -- a frontend must never hard-code it, and
// neither does this adapter.
DebugTargetInfo SimulatorDebugTarget::info() const {
    const Simulator::MemoryMapInfo mm = Simulator::memoryMap();
    DebugTargetInfo i{};
    i.targetName = "STM32G431RBT6";
    i.boardName = "CT117E-M4";
    i.architecture = "ARM Cortex-M4F";
    i.littleEndian = true;
    i.flashBase = mm.flashBase;
    i.flashSize = mm.flashSize;
    i.flashAliasBase = mm.aliasBase;
    i.flashAliasSize = mm.aliasSize;
    i.sramBase = mm.sramBase;
    i.sramSize = mm.sramSize;
    i.ccmBase = mm.ccmBase;
    i.ccmSize = mm.ccmSize;
    // capabilities of THIS implementation (stage 7-2A)
    i.exactSingleStep = sim_.exactStepSupported();
    i.executionBreakpoint = true;
    i.dataWatchpoint = false;   // not in this stage
    i.fpu = true;               // cortex-m4f model, S0-S31/FPSCR readable
    i.flashProgramming = true;  // IFlashProgrammer (separate from writeMemory!)
    i.asyncStopEvent = true;    // Simulator::setStopObserver -> TARGET_STOPPED
    return i;
}

// ---------------------------------------------------------------------------
// execution control
// ---------------------------------------------------------------------------
DebugStatus SimulatorDebugTarget::halt() {
    checkThread();
    if (!sim_.isRunning()) return DebugStatus::Ok;  // safe no-op
    // pause() freezes the WHOLE virtual board: the run loop stops, so the CPU,
    // SysTick, timers, PWM/capture timing, pulse sources, USART, ADC, the
    // board's signal monitor and the virtual clock all stop together.
    sim_.pause();
    uint32_t pc = 0;
    if (sim_.cpuReadRegister(CpuReg::PC, pc)) {
        sim_.debugLogLine(
            QString("halt pc=0x%1").arg(pc & ~1u, 8, 16, QChar('0')));
    }
    return DebugStatus::Ok;
}

DebugStatus SimulatorDebugTarget::resume() {
    checkThread();
    if (!sim_.firmwareLoaded()) return DebugStatus::InvalidState;
    if (sim_.isRunning()) return DebugStatus::Ok;  // already running: no-op
    uint32_t pc = 0;
    const bool havePc = sim_.cpuReadRegister(CpuReg::PC, pc);
    // startRun() re-establishes the 1:1 real-time pacing baseline from the
    // current cycle count, so wall time spent halted is NEVER replayed, and it
    // applies the continue-from-breakpoint skip when needed.
    sim_.startRun();
    if (!sim_.isRunning()) return DebugStatus::InvalidState;
    if (havePc) {
        sim_.debugLogLine(
            QString("resume pc=0x%1").arg(pc & ~1u, 8, 16, QChar('0')));
    }
    return DebugStatus::Ok;
}

DebugStatus SimulatorDebugTarget::step() {
    checkThread();
    if (sim_.isRunning()) return DebugStatus::InvalidState;  // Halted only
    if (!sim_.firmwareLoaded()) return DebugStatus::InvalidState;
    return sim_.debugStepOne() ? DebugStatus::Ok : DebugStatus::InvalidState;
}

DebugStatus SimulatorDebugTarget::resetHalt() {
    checkThread();
    return sim_.debugResetHalt() ? DebugStatus::Ok : DebugStatus::CpuError;
}

DebugStatus SimulatorDebugTarget::resetRun() {
    checkThread();
    if (sim_.debugResetHalt() == false) return DebugStatus::CpuError;
    return resume();
}

// ---------------------------------------------------------------------------
// register access
// ---------------------------------------------------------------------------
DebugStatus SimulatorDebugTarget::readRegister(DebugRegister reg,
                                               uint64_t& value) {
    checkThread();
    CpuReg cpuReg{};
    if (!toCpuReg(reg, cpuReg)) return DebugStatus::InvalidRegister;
    uint32_t v = 0;
    if (!sim_.cpuReadRegister(cpuReg, v)) {
        // the engine does not implement / rejects this register on this build
        return DebugStatus::Unsupported;
    }
    value = v;
    return DebugStatus::Ok;
}

DebugStatus SimulatorDebugTarget::writeRegister(DebugRegister reg,
                                                uint64_t value) {
    checkThread();
    CpuReg cpuReg{};
    if (!toCpuReg(reg, cpuReg)) return DebugStatus::InvalidRegister;
    if (!sim_.cpuWriteRegister(cpuReg, uint32_t(value))) {
        return DebugStatus::Unsupported;
    }
    return DebugStatus::Ok;
}

// ---------------------------------------------------------------------------
// memory access
// ---------------------------------------------------------------------------
DebugStatus SimulatorDebugTarget::readMemory(uint32_t address, void* data,
                                             size_t size) {
    // the peripheral space is read through the real bus model, which touches
    // the SoC state -> owner thread required even for reads
    checkThread();
    if (!data || size == 0) return DebugStatus::InvalidAddress;
    return sim_.guestRead(address, data, size);
}

DebugStatus SimulatorDebugTarget::writeMemory(uint32_t address,
                                              const void* data, size_t size) {
    checkThread();
    if (!data || size == 0) return DebugStatus::InvalidAddress;
    return sim_.guestWrite(address, data, size);
}

// ---------------------------------------------------------------------------
// breakpoints
// ---------------------------------------------------------------------------
DebugStatus SimulatorDebugTarget::addBreakpoint(uint32_t address) {
    checkThread();
    sim_.debugAddBreakpoint(address);
    return DebugStatus::Ok;
}

DebugStatus SimulatorDebugTarget::removeBreakpoint(uint32_t address) {
    checkThread();
    sim_.debugRemoveBreakpoint(address);
    return DebugStatus::Ok;
}

void SimulatorDebugTarget::clearBreakpoints() {
    checkThread();
    sim_.debugClearBreakpoints();
}

// ---------------------------------------------------------------------------
// firmware
// ---------------------------------------------------------------------------
DebugStatus SimulatorDebugTarget::loadFirmware(const std::string& path) {
    checkThread();
    // Keeps the stage-6 behaviour: a load while running auto-pauses first.
    if (!sim_.loadFirmware(QString::fromStdString(path))) {
        // rejected image (bad path / bad hex); the reason is in the session log
        return DebugStatus::CpuError;
    }
    return DebugStatus::Ok;
}