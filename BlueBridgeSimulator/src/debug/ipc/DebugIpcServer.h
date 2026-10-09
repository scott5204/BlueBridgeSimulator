#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "debug/DebugTypes.h"
#include "debug/ipc/DebugCommand.h"

class IDebugTarget;
class IFlashProgrammer;

// ============================================================================
// DebugIpcServer -- stage 7-2A Windows Named Pipe debug IPC server.
//
//                          future Keil AGDI DLL / test client
//                                       |
//                              \\.\pipe\<...> (byte mode, 24-byte header)
// =============================================|=============================
//                              DebugIpcServer
//                                       |
//                    +------------------+-------------------+
//                    |                                      |
//        pipe worker thread (NEVER touches            DebugCommandQueue
//        Simulator/Unicorn/STM32/Board:                (thread-safe)
//        CreateNamedPipe / ConnectNamedPipe /                  |
//        ReadFile / WriteFile / parse / encode /     owner pump (processPending
//        enqueue / wait response / emit events)       Commands) on the simulator
//                    ^                                  owner thread only
//                    |                                         |
//              async event queue                        IDebugTarget +
//              (Simulator stop observer)                IFlashProgrammer
//                                                             |
//                                                         Simulator -> unicorn
//
// Hard thread contract (verified by the tests, spec 5/36/159):
//   * the worker ONLY does pipe I/O + wire encode/decode + queue/slot handling,
//   * every debugger command executes on the simulator's owner thread inside
//     processPendingCommands() -- including session cleanup on disconnect
//     (the worker enqueues a cleanup command, it never halts the target),
//   * the owner thread never waits for the pipe thread (events are queued),
//   * all mutating debug APIs keep their stage-7-1 owner-thread assertions.
//
// Not started at all unless --debug-pipe was given (spec 10/139).
// ============================================================================
class DebugIpcServer {
public:
    struct Options {
        // May be given with or without the \\.\pipe\ prefix. Never a fixed
        // project-global name: two simulators / two debuggers must not collide
        // (spec 8).
        std::string pipeName;
        // --debug-ipc-trace: one line per packet (opcode/requestId/size/status).
        // Never dumps memory payloads; off by default (spec 116).
        bool trace = false;
        int requestTimeoutMs = 2000;
        int programEndTimeoutMs = 10000;
    };

    explicit DebugIpcServer(Options options);
    ~DebugIpcServer();

    DebugIpcServer(const DebugIpcServer&) = delete;
    DebugIpcServer& operator=(const DebugIpcServer&) = delete;

    // Creates the pipe + starts the worker thread. Returns false when the pipe
    // cannot be created (e.g. the name is taken): the GUI keeps working and the
    // reason is logged as "[ipc] Failed to create pipe ..." (spec 138).
    bool start();
    // Stops accepting, cancels pending I/O, joins the worker (spec 118).
    void stop();
    bool running() const;

    // ---- owner (simulator) thread -----------------------------------------
    // Executes queued commands against the target/programmer. MUST run on the
    // thread that called start(); call it once per owner-loop tick. It is safe
    // (and required) while the target is Halted -- otherwise STEP / READ_MEMORY
    // / RESET after a debugger halt would deadlock (spec 39/40).
    // Work is budgeted (a few commands / ~2 ms), control commands first
    // (spec 111/112), so a busy debugger can never starve the simulation.
    void processPendingCommands(IDebugTarget& target,
                                IFlashProgrammer& programmer);
    // Bounded wait for the headless owner loop (never blocks the GUI for long).
    void waitForCommands(int timeoutMs) const;

    // ---- async stop events -------------------------------------------------
    // Called on the owner thread from the Simulator's stop observer with the
    // Simulator's StopInfo (the single source of truth -- the IPC layer never
    // guesses "I just sent STEP, so the reason must be SingleStep"). Queues a
    // TARGET_STOPPED / TARGET_FAULTED event; never sent before HELLO and never
    // carried over into the next session.
    void notifyTargetStopped(const StopInfo& info, uint64_t virtualCycles);

    // ---- session / status (any thread; atomics only) ----------------------
    bool clientConnected() const;
    uint64_t sessionId() const;
    uint64_t sessionsServed() const;

    // Owner-thread log sink: [ipc]/[program] lines are queued by the worker and
    // drained inside processPendingCommands, so the session file logger stays
    // single-threaded.
    void setLogSink(std::function<void(const std::string&)> sink);
    // Called on the WORKER thread when a session completes HELLO (true) or
    // disconnects (false). Must therefore be atomic-only work (see
    // Simulator::setDebuggerAttached).
    void setAttachObserver(std::function<void(bool)> observer);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};