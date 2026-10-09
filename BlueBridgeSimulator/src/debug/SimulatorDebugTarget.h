#pragma once

#include <thread>

#include "debug/IDebugTarget.h"

class Simulator;

// ============================================================================
// SimulatorDebugTarget - IDebugTarget over the existing Simulator.
//
// This is the adapter a future AGDI DLL / CLI / GDB stub speaks to. It never
// touches the internals directly:
//
//     IDebugTarget
//          |
//     SimulatorDebugTarget        <- DebugRegister -> CpuReg mapping only
//          |
//     Simulator                   <- owns state (StopInfo / running / faulted)
//       /       \
//     SoC      Board
//       |
//     ICpuEngine -> UnicornCpu -> unicorn.dll
//
//   * no uc_engine*, UnicornApi, Stm32G431*, Timer*, GpioPort* members here
//   * the target state is READ from the Simulator, never duplicated
//   * all calls must come from the simulator's owner thread (unicorn.dll is
//     single-threaded on Windows); debug builds assert it
// ============================================================================
class SimulatorDebugTarget : public IDebugTarget {
public:
    explicit SimulatorDebugTarget(Simulator& sim);

    // ---- state -----------------------------------------------------------
    TargetState state() const override;
    StopInfo stopInfo() const override;
    uint64_t virtualCycles() const override;
    bool firmwareLoaded() const override;
    DebugTargetInfo info() const override;

    // ---- execution control ----------------------------------------------
    DebugStatus halt() override;
    DebugStatus resume() override;
    DebugStatus step() override;
    DebugStatus resetHalt() override;
    DebugStatus resetRun() override;

    // ---- register access -------------------------------------------------
    DebugStatus readRegister(DebugRegister reg, uint64_t& value) override;
    DebugStatus writeRegister(DebugRegister reg, uint64_t value) override;

    // ---- memory access ---------------------------------------------------
    DebugStatus readMemory(uint32_t address, void* data, size_t size) override;
    DebugStatus writeMemory(uint32_t address, const void* data,
                            size_t size) override;

    // ---- execution breakpoints -------------------------------------------
    DebugStatus addBreakpoint(uint32_t address) override;
    DebugStatus removeBreakpoint(uint32_t address) override;
    void clearBreakpoints() override;

    // ---- firmware --------------------------------------------------------
    DebugStatus loadFirmware(const std::string& path) override;

private:
    // Asserts the caller runs on the simulator's owner thread (debug builds
    // only; see the class comment).
    void checkThread() const;

    Simulator& sim_;
    std::thread::id ownerThread_;
};