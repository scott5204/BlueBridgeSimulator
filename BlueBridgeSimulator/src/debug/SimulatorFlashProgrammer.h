#pragma once

#include <atomic>
#include <thread>
#include <vector>

#include "debug/IFlashProgrammer.h"

class Simulator;

// ============================================================================
// SimulatorFlashProgrammer -- IFlashProgrammer over the existing Simulator.
//
//   IFlashProgrammer
//        |
//   SimulatorFlashProgrammer     <- staging image + transaction state only
//        |
//   Simulator                    <- the single live flash + reset/commit path
//        |
//   Stm32G431 (flash backing) -> unicorn mapped memory
//
// Hard properties (stage 7-2A):
//   * the debug layer never memcpy's unicorn memory -- the staging image is
//     filled from Simulator::readFlashImage() and committed through
//     Simulator::replaceFlashImage() (the SAME path as loadFirmware),
//   * live flash is touched exactly once, in programEnd(); erase/write/edit
//     only ever modify the staging copy (an abort can therefore never corrupt
//     the running firmware),
//   * one transaction at a time; a disconnect aborts (never commits half an
//     image),
//   * owner thread only (unicorn.dll is single-threaded). Debug builds assert.
// ============================================================================
class SimulatorFlashProgrammer : public IFlashProgrammer {
public:
    explicit SimulatorFlashProgrammer(Simulator& sim);

    DebugStatus programBegin(uint64_t& token) override;
    DebugStatus programErase(uint64_t token, uint32_t address,
                             uint32_t size) override;
    DebugStatus programWrite(uint64_t token, uint32_t address, const void* data,
                             size_t size) override;
    DebugStatus programEnd(uint64_t token) override;
    DebugStatus programAbort(uint64_t token) override;

    bool programActive() const override { return active_; }
    void abortActive() override;
    uint32_t maxTransfer() const override;

    // Session salt for the program token (stage 7-2A spec 140: a monotonic
    // 64-bit counter salted with the session id). Atomic: written by the IPC
    // pipe thread on connect, read by the owner thread.
    void setSessionSalt(uint32_t sessionId) {
        salt_.store(sessionId ? sessionId : 1u);
    }

private:
    void checkThread() const;
    uint64_t makeToken();
    // Physical flash range only (the 0x00000000 alias is a read mirror and is
    // never a valid programming target); 64-bit math catches wraparound.
    bool rangeOk(uint32_t address, uint32_t size) const;
    void discard(const char* why);

    Simulator& sim_;
    std::thread::id ownerThread_;

    std::vector<uint8_t> staging_;   // transaction copy of the live image
    uint64_t token_ = 0;
    bool active_ = false;
    uint64_t tokenCounter_ = 0;
    std::atomic<uint32_t> salt_{1u};
};