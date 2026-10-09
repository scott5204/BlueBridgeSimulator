#include "debug/SimulatorFlashProgrammer.h"

#include <cassert>
#include <cstring>

#include "debug/ipc/DebugIpcWire.h"
#include "sim/Simulator.h"

SimulatorFlashProgrammer::SimulatorFlashProgrammer(Simulator& sim)
    : sim_(sim), ownerThread_(std::this_thread::get_id()) {}

void SimulatorFlashProgrammer::checkThread() const {
#ifndef NDEBUG
    assert(std::this_thread::get_id() == ownerThread_ &&
           "SimulatorFlashProgrammer: call must run on the simulator's owner "
           "thread (unicorn.dll is single-threaded)");
#endif
}

uint64_t SimulatorFlashProgrammer::makeToken() {
    // Monotonic 64-bit transaction counter in the high half, session salt in
    // the low half (never 0, never a fixed 1 -- spec 140).
    const uint64_t counter = ++tokenCounter_;
    return (counter << 32) | uint64_t(salt_.load());
}

uint32_t SimulatorFlashProgrammer::maxTransfer() const {
    return bbipc::kMaxProgramTransfer;
}

bool SimulatorFlashProgrammer::rangeOk(uint32_t address, uint32_t size) const {
    if (size == 0) return false;
    const Simulator::MemoryMapInfo mm = Simulator::memoryMap();
    const uint64_t lo = mm.flashBase;
    const uint64_t hi = uint64_t(mm.flashBase) + mm.flashSize;
    const uint64_t end = uint64_t(address) + uint64_t(size);
    return address >= lo && end <= hi;
}

void SimulatorFlashProgrammer::discard(const char* why) {
    staging_.clear();
    staging_.shrink_to_fit();
    active_ = false;
    sim_.debugLog(QString("[program] %1 token=0x%2 (live flash unchanged)")
                      .arg(why)
                      .arg(token_, 16, 16, QChar('0')));
    token_ = 0;
}

DebugStatus SimulatorFlashProgrammer::programBegin(uint64_t& token) {
    checkThread();
    // Programming requires a HALTED target and never auto-halts (spec 50):
    // the debugger's sequence is HALT -> PROGRAM_BEGIN, which is predictable.
    if (sim_.isRunning()) return DebugStatus::InvalidState;
    if (active_) return DebugStatus::ProgramAlreadyActive;
    if (!sim_.readFlashImage(staging_)) return DebugStatus::CpuError;
    token_ = makeToken();
    active_ = true;
    token = token_;
    sim_.debugLog(QString("[program] begin token=0x%1 staging=%2 KiB "
                          "(copy of the live flash)")
                      .arg(token_, 16, 16, QChar('0'))
                      .arg(staging_.size() / 1024));
    return DebugStatus::Ok;
}

DebugStatus SimulatorFlashProgrammer::programErase(uint64_t token,
                                                   uint32_t address,
                                                   uint32_t size) {
    checkThread();
    if (!active_) return DebugStatus::ProgramNotActive;
    if (token != token_) return DebugStatus::ProgramTokenInvalid;
    if (size == 0) return DebugStatus::Ok;  // empty transfer = Ok no-op
    if (!rangeOk(address, size)) {
        sim_.debugLog(QString("[program] erase rejected: 0x%1 +%2 outside the "
                              "physical flash range")
                          .arg(address, 8, 16, QChar('0'))
                          .arg(size));
        return DebugStatus::ProgramRangeInvalid;
    }
    std::memset(staging_.data() + (address - Simulator::memoryMap().flashBase),
                0xFF, size);
    sim_.debugLog(QString("[program] erase 0x%1 +%2 (staging only)")
                      .arg(address, 8, 16, QChar('0'))
                      .arg(size));
    return DebugStatus::Ok;
}

DebugStatus SimulatorFlashProgrammer::programWrite(uint64_t token,
                                                   uint32_t address,
                                                   const void* data,
                                                   size_t size) {
    checkThread();
    if (!active_) return DebugStatus::ProgramNotActive;
    if (token != token_) return DebugStatus::ProgramTokenInvalid;
    if (!data) return DebugStatus::ProgramRangeInvalid;
    if (size == 0) return DebugStatus::Ok;  // empty transfer = Ok no-op
    if (size > maxTransfer()) return DebugStatus::ProgramRangeInvalid;
    if (size > 0xFFFFFFFFull || !rangeOk(address, uint32_t(size))) {
        sim_.debugLog(QString("[program] write rejected: 0x%1 +%2 outside the "
                              "physical flash range")
                          .arg(address, 8, 16, QChar('0'))
                          .arg(qulonglong(size)));
        return DebugStatus::ProgramRangeInvalid;
    }
    // Out-of-order and overlapping writes are explicitly allowed; the last
    // write wins (spec 56/57). No 1->0 physical programming restriction.
    std::memcpy(staging_.data() + (address - Simulator::memoryMap().flashBase),
                data, size);
    sim_.debugLog(QString("[program] write 0x%1 +%2 (staging only)")
                      .arg(address, 8, 16, QChar('0'))
                      .arg(qulonglong(size)));
    return DebugStatus::Ok;
}

DebugStatus SimulatorFlashProgrammer::programEnd(uint64_t token) {
    checkThread();
    if (!active_) return DebugStatus::ProgramNotActive;
    if (token != token_) return DebugStatus::ProgramTokenInvalid;
    const QString tokenText = QString("0x%1").arg(token, 16, 16, QChar('0'));
    // Atomic commit through the SAME path the firmware loader uses: replace the
    // single live flash backing store, drop the translated code, reset and
    // stay Halted (StopReason::Reset, SP/PC from the NEW vector table).
    if (!sim_.replaceFlashImage(staging_)) {
        // internal error: auto-abort, never leave a half-open transaction
        discard("commit FAILED -> auto-abort");
        return DebugStatus::CpuError;
    }
    active_ = false;
    staging_.clear();
    staging_.shrink_to_fit();
    sim_.debugLog(QString("[program] commit token=%1 ok -> reset, target "
                          "halted (StopReason Reset)")
                      .arg(tokenText));
    token_ = 0;
    return DebugStatus::Ok;
}

DebugStatus SimulatorFlashProgrammer::programAbort(uint64_t token) {
    checkThread();
    if (!active_) return DebugStatus::ProgramNotActive;
    if (token != token_) return DebugStatus::ProgramTokenInvalid;
    discard("abort");
    return DebugStatus::Ok;
}

void SimulatorFlashProgrammer::abortActive() {
    checkThread();
    if (!active_) return;
    discard("abort (session teardown)");
}