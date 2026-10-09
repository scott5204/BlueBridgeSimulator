#pragma once

// ============================================================================
// Debug IPC command + result plumbing (pure C++, no Qt, no Win32).
//
// A decoded debugger request travels from the Named Pipe worker thread to the
// simulator's owner thread as a DebugCommand:
//
//     pipe worker:  ReadFile -> validate -> decode -> push(queue)
//                        ...  wait on the command's CommandSlot (bounded)
//     owner thread: pop(queue) -> execute against IDebugTarget/IFlashProgrammer
//                        -> slot->complete(status, payload)
//
// Hard rules (stage 7-2A spec):
//   * NOTHING that points into the pipe buffer crosses the thread boundary --
//     blobs are copied into std::vector<uint8_t> (DebugCommandArgs::bytes),
//   * the worker never calls IDebugTarget / IFlashProgrammer / Simulator /
//     Unicorn, it only ever touches the queue + the slot,
//   * a timed-out request is marked abandoned: the owner skips it if it is
//     still queued, so the client can never receive a stale response (its
//     requestId was already answered with kStTimeout).
// ============================================================================

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "debug/ipc/DebugIpcWire.h"

// Decoded request arguments. The pipe worker decodes the wire payload into this
// plain-value form; the owner-thread executor reads the fields its opcode needs.
// Field meaning per opcode is documented in docs/debug_ipc_protocol.md and
// enforced by DebugIpcCodec usage in DebugIpcServer.cpp.
struct DebugCommandArgs {
    uint32_t u32a = 0;  // address / registerId / count / flags
    uint32_t u32b = 0;  // length / size / register value low word
    uint64_t u64a = 0;  // token / cookie / register value
    std::vector<uint8_t> bytes;   // memory / program blobs (copied)
    std::vector<uint32_t> ids;    // batch register ids
};

// Shared result slot: written by the owner thread, awaited by the pipe worker.
class CommandSlot {
public:
    void complete(bbipc::WireStatus status, std::vector<uint8_t> payload);
    // Waits up to @timeoutMs for the owner thread. Returns false on timeout or
    // when the command was abandoned in the meantime.
    bool wait(std::vector<uint8_t>& payload, bbipc::WireStatus& status,
              int timeoutMs);
    // "Nobody is waiting for this result any more" (worker timeout / session
    // teardown). Wakes a waiter and makes the owner skip pending execution.
    void abandon();
    bool abandoned() const;
    bool done() const;

private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool done_ = false;
    bool abandoned_ = false;
    bbipc::WireStatus status_ = bbipc::kStOk;
    std::vector<uint8_t> payload_;
};

struct DebugCommand {
    uint32_t requestId = 0;
    uint16_t opcode = 0;
    bool control = false;  // priority class (HALT/RESET/STEP/RESUME/HELLO)
    DebugCommandArgs args;
    std::shared_ptr<CommandSlot> slot;
};

// Control commands jump the queue so a debugger "Stop" is never stuck behind a
// burst of memory reads (spec 111/112).
bool isControlOpcode(uint16_t opcode);

// ---------------------------------------------------------------------------
// Thread-safe, bounded-work command queue. The owner pump drains it with a
// command/time budget, so a hostile (or just very busy) debugger can never
// starve the simulation or the GUI.
// ---------------------------------------------------------------------------
class DebugCommandQueue {
public:
    void push(DebugCommand cmd);
    // Control commands first, then normal ones. Returns false when empty.
    bool tryPop(DebugCommand& out);
    // Bounded wait for the headless owner loop (never called by the worker).
    void waitForWork(int timeoutMs) const;
    size_t pending() const;
    // Drops everything, abandoning each command's slot (shutdown / disconnect).
    void clear();

private:
    mutable std::mutex m_;
    mutable std::condition_variable cv_;
    std::deque<DebugCommand> control_;
    std::deque<DebugCommand> normal_;
};