#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "debug/DebugTypes.h"

// ============================================================================
// IDebugTarget - the debugger-facing abstraction of the virtual MCU board.
//
// This is the interface a future BlueBridgeAGDI.dll (Keil uVision), a CLI tool
// or a GDB stub speaks to. It is deliberately:
//   * pure C++ (no Qt, no unicorn, no STM32 / board types),
//   * synchronous (one call = one completed operation; the debugger drives
//     time, the target never blocks),
//   * callable only from the simulator's owner thread (unicorn.dll on Windows
//     is single-threaded; an AGDI/IPC frontend must marshal its commands onto
//     that thread -- stage 7-2 does that, not this stage).
//
// Concurrency contract for every implementation: all methods must be called on
// the thread that owns the simulator/CPU engine.
// ============================================================================
class IDebugTarget {
public:
    virtual ~IDebugTarget() = default;

    // ---- state -----------------------------------------------------------
    virtual TargetState state() const = 0;
    virtual StopInfo stopInfo() const = 0;
    // Virtual HCLK cycles since the last reset (the model's time base; used by
    // GET_STATE and the TARGET_STOPPED event payload).
    virtual uint64_t virtualCycles() const = 0;
    virtual bool firmwareLoaded() const = 0;
    // Static facts (capabilities / memory map) a frontend must not hard-code.
    virtual DebugTargetInfo info() const = 0;

    // ---- execution control ----------------------------------------------
    // All control calls are safe no-ops in a state where they make no sense
    // (halt() while halted, resume() while running) and never fault the target.
    virtual DebugStatus halt() = 0;
    virtual DebugStatus resume() = 0;
    virtual DebugStatus step() = 0;       // exactly one guest instruction
    virtual DebugStatus resetHalt() = 0;  // reset, then stay halted
    virtual DebugStatus resetRun() = 0;   // reset, then run

    // ---- register access -------------------------------------------------
    virtual DebugStatus readRegister(DebugRegister reg, uint64_t& value) = 0;
    virtual DebugStatus writeRegister(DebugRegister reg, uint64_t value) = 0;

    // ---- memory access ---------------------------------------------------
    // Flash (incl. the 0x00000000 boot alias), SRAM and CCM are accessed
    // byte-for-byte as the CPU sees them. The peripheral space is accessed
    // through the normal bus model, so a debug read of e.g. USART_RDR has the
    // same side effects as a CPU read (documented in PROJECT_HANDOFF.md).
    virtual DebugStatus readMemory(uint32_t address, void* data,
                                   size_t size) = 0;
    virtual DebugStatus writeMemory(uint32_t address, const void* data,
                                    size_t size) = 0;

    // ---- execution breakpoints -------------------------------------------
    // Host-side breakpoints; the guest flash bytes are never modified. The
    // address is normalized (Thumb bit cleared) on entry.
    virtual DebugStatus addBreakpoint(uint32_t address) = 0;
    virtual DebugStatus removeBreakpoint(uint32_t address) = 0;
    virtual void clearBreakpoints() = 0;

    // ---- firmware --------------------------------------------------------
    // Loading while running auto-pauses first (never silently refused).
    virtual DebugStatus loadFirmware(const std::string& path) = 0;
};