#pragma once

#include <cstddef>
#include <cstdint>

#include "debug/DebugTypes.h"

// ============================================================================
// IFlashProgrammer -- the debugger *programming* path (stage 7-2A).
//
// This is deliberately a SEPARATE interface from IDebugTarget::writeMemory:
//
//   * IDebugTarget::writeMemory()  = debugger memory access. Writing flash
//     through it stays DebugStatus::Unsupported forever -- a Keil Memory
//     Window edit must never (re)flash the board behind the user's back.
//   * IFlashProgrammer             = "Download / Load Application" only. It
//     works on a staging copy and only a successful programEnd() commits.
//
// Transaction model (all calls are synchronous, owner-thread only):
//
//   programBegin()  -> token, staging = copy of the current virtual flash
//   programErase()  -> staging[range] = 0xFF          (any order, overlap ok)
//   programWrite()  -> staging[range] = data          (any order, overlap ok)
//   programEnd()    -> atomic commit: live flash := staging, translated code
//                      dropped, target reset and left HALTED (StopReason Reset)
//   programAbort()  -> staging dropped, live flash untouched
//
// One transaction at a time per target; a disconnecting debugger must abort
// (never commit half an image). Addresses are always PHYSICAL flash addresses
// (0x08000000...): the 0x00000000 boot alias is a read mirror and is never a
// valid programming target.
// ============================================================================
class IFlashProgrammer {
public:
    virtual ~IFlashProgrammer() = default;

    // Begins a transaction. Requires a HALTED target (never auto-halts):
    // returns InvalidState while the target is running.
    virtual DebugStatus programBegin(uint64_t& token) = 0;
    // Fills [address, address+size) with 0xFF in the staging image.
    // size == 0 is a no-op (Ok), addresses outside the physical flash range
    // (including the alias) are ProgramRangeInvalid.
    virtual DebugStatus programErase(uint64_t token, uint32_t address,
                                     uint32_t size) = 0;
    // Overwrites [address, address+size) in the staging image (out-of-order and
    // overlapping writes are allowed; the last write wins). No 1->0 physical
    // programming restriction: this is the debugger programming path, not a
    // flash controller model.
    virtual DebugStatus programWrite(uint64_t token, uint32_t address,
                                     const void* data, size_t size) = 0;
    // Validates the token, commits the staging image, drops translated code,
    // resets the target and leaves it Halted with StopReason::Reset.
    virtual DebugStatus programEnd(uint64_t token) = 0;
    // Discards the staging image. Live flash is never touched by an abort.
    virtual DebugStatus programAbort(uint64_t token) = 0;

    // True while a transaction is open (session cleanup / tests).
    virtual bool programActive() const = 0;
    // Aborts whatever transaction is open (disconnect cleanup); no-op otherwise.
    virtual void abortActive() = 0;
    // Maximum bytes accepted by a single erase/write call.
    virtual uint32_t maxTransfer() const = 0;
};