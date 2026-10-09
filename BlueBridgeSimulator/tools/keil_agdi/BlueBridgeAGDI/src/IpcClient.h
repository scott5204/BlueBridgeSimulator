#pragma once
/*
 * BlueBridge Debug IPC client (stage 7-2B.2 sections 10-23).
 *
 * Transport only: knows the wire protocol, the named pipe and the thread model
 * -- nothing about Keil, AGDI, Qt or the simulator.
 *
 * Thread model (the single most important rule, section 11):
 *   * exactly ONE thread calls ReadFile() -- the reader thread created by
 *     connect();
 *   * every AGDI caller allocates a requestId, registers a PendingRequest,
 *     writes under a mutex and waits for completion;
 *   * responses are matched by requestId (order is never assumed);
 *   * async events go to an event queue (B.2 only logs/queues them).
 */

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "IpcProtocol.h"
#include "IpcTypes.h"

namespace bbx {

class IpcClient {
public:
    IpcClient();
    ~IpcClient();
    IpcClient(const IpcClient &) = delete;
    IpcClient &operator=(const IpcClient &) = delete;

    /* ---- lifecycle ------------------------------------------------------ */
    /* Opens `pipeName` (with or without the \\.\pipe\ prefix), retrying until
     * `timeoutMs` elapses -- a simulator that is still starting up is normal. */
    bool connect(const std::wstring &pipeName, int timeoutMs);
    /* shuttingDown -> CancelIoEx -> reader join -> pending = ConnectionLost ->
     * CloseHandle (section 18). Never detaches the thread. */
    void disconnect();
    bool connected() const;

    std::wstring lastErrorCopy() const;      // thread-safe copy accessor
    const std::wstring &pipeName() const { return pipeName_; }

    void setRequestTimeoutMs(int ms) { requestTimeoutMs_ = ms > 0 ? ms : 2000; }
    int requestTimeoutMs() const { return requestTimeoutMs_; }

    /* ---- requests (section 10) ------------------------------------------ */
    RequestResult hello(uint32_t clientType, uint32_t clientPid);
    RequestResult ping(uint64_t cookie);
    RequestResult getCapabilities(TargetCaps *out);
    RequestResult getState(TargetState *out);
    RequestResult getStopInfo(uint32_t *reason, uint32_t *pc);

    RequestResult readRegister(uint32_t id, uint64_t *value);
    RequestResult writeRegister(uint32_t id, uint64_t value);
    RequestResult readRegisters(const std::vector<uint32_t> &ids,
                                std::vector<RegisterValue> *out);
    RequestResult writeRegisters(const std::vector<RegisterValue> &in,
                                 std::vector<RegisterValue> *out);

    RequestResult readMemory(uint32_t address, uint32_t length,
                             std::vector<uint8_t> *out);
    RequestResult writeMemory(uint32_t address, const uint8_t *data,
                              size_t length);

    RequestResult resetHalt();
    RequestResult resetRun();
    RequestResult step();
    RequestResult halt();
    RequestResult resume();

    /* B.3 transport helpers -- present, but not wired to the AGDI UI in B.2 */
    RequestResult addBreakpoint(uint32_t address);
    RequestResult removeBreakpoint(uint32_t address);
    RequestResult clearBreakpoints();

    /* virtual flash programming (used by the probe; the AGDI driver keeps the
     * ordinary memory-write path away from flash, section 52) */
    RequestResult programBegin(uint64_t *token, uint32_t *flashBase,
                               uint32_t *flashSize);
    RequestResult programErase(uint64_t token, uint32_t address, uint32_t size);
    RequestResult programWrite(uint64_t token, uint32_t address,
                               const uint8_t *data, size_t length);
    RequestResult programEnd(uint64_t token, uint32_t *initialSp,
                             uint32_t *resetPc);
    RequestResult programAbort(uint64_t token);

    /* ---- async events (section 23) --------------------------------------- */
    bool popEvent(IpcEvent &ev);
    size_t queuedEvents() const;

    /* ---- async events: B.3 run-control path ------------------------------ *
     * The queue above is a diagnostic ring (it may drop old entries); the
     * waiter below is the AUTHORITATIVE path for "the target stopped":
     *   * latestEventSequence()  -- baseline recorded BEFORE RESUME;
     *   * waitForEventAfter()    -- blocks until an event with a HIGHER
     *     sequence arrives, or the session ends. It never consumes events, so
     *     two waiters cannot steal each other's stop, and a stale event left
     *     over from a previous step/reset can never complete a new run.
     * `timeoutMs <= 0` waits indefinitely (an execution wait is NOT an RPC
     * timeout, spec section 137). The wait is woken by: new event, connection
     * loss, disconnect(). */
    uint64_t latestEventSequence() const;
    EventWaitResult waitForEventAfter(uint64_t baselineSequence, TargetEvent *out,
                                      int timeoutMs);
    /* Total events seen since process start (diagnostics). */
    uint64_t totalEvents() const { return nEvents_.load(); }

    /* ---- run-stop wait (B.3 sections 16-18 / 52-53) ---------------------- *
     * A stop event can be DELIVERED after the response of the command that
     * produced it (measured with the probe: a STEP response reaches the caller
     * before the STEP's own SingleStep event is parsed). A run waiter that only
     * looked at sequence numbers would therefore consume that leftover event
     * and report "stopped" right after RESUME.
     *
     * StopBaseline captures the target's stop state (reason/pc/cycles) plus the
     * event sequence BEFORE RESUME is sent. waitForRunStop() then ignores a
     * candidate event that merely REPEATS that state -- such an event can only
     * be the leftover of the previous operation. A repeated candidate is only
     * accepted after one GET_STATE confirmed the target really is halted (the
     * simulator clears its stop state on RESUME, so a halted target with an
     * identical tuple is a genuine stop; the register refresh afterwards reads
     * the live PC anyway). */
    struct StopBaseline {
        bool     valid = false;      // false: no stop state before the run
        uint32_t reason = 0;
        uint32_t pc = 0;
        uint64_t cycles = 0;
        uint64_t eventSequence = 0;
    };
    /* GET_STATE + latestEventSequence(); call right before RESUME. */
    StopBaseline captureStopBaseline();
    /* Waits for the stop that ends the run started with `base`. `timeoutMs <= 0`
     * waits indefinitely. Never consumes events (several waiters are safe). */
    EventWaitResult waitForRunStop(const StopBaseline &base, int timeoutMs,
                                   TargetEvent *out);

    struct Stats {
        unsigned long long requests = 0;
        unsigned long long timeouts = 0;
        unsigned long long staleResponses = 0;
        unsigned long long events = 0;
        unsigned long long protocolErrors = 0;
    };
    Stats stats() const;

private:
    struct Pending {
        uint32_t id = 0;
        bool done = false;
        uint32_t status = bbipc::kStOk;
        std::vector<uint8_t> payload;
    };

    RequestResult request(uint16_t opcode, const std::vector<uint8_t> &payload,
                          int timeoutMs);
    uint32_t allocRequestId();

    static unsigned __stdcall readerTrampoline(void *self);
    void readerLoop();
    bool readExact(uint8_t *buf, size_t n);
    /* serialized write with an absolute deadline; a write that cannot be
     * flushed in time cancels its own I/O and marks the session lost (the
     * stream would be corrupt from then on). */
    bool writeAll(const uint8_t *buf, size_t n, int timeoutMs);

    void completePending(uint32_t id, uint32_t status,
                         const std::vector<uint8_t> &payload);
    void queueEvent(const IpcEvent &ev);
    /* Wakes every waitForEventAfter() waiter; the waiter re-checks the
     * connected/shuttingDown flags and returns ConnectionLost/Shutdown. */
    void wakeEventWaiters();
    void failAllPending(const std::wstring &reason);
    void markConnectionLost(const std::wstring &reason);
    void setLastError(const std::wstring &text);
    void closePipeHandle();

    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE readerThread_ = NULL;
    std::atomic<bool> connected_{false};
    std::atomic<bool> shuttingDown_{false};
    std::wstring pipeName_;

    std::atomic<uint32_t> nextRequestId_{1};
    std::mutex writeMutex_;
    std::mutex pendingMutex_;
    std::condition_variable pendingCv_;
    std::map<uint32_t, std::shared_ptr<Pending>> pending_;

    mutable std::mutex eventMutex_;
    std::condition_variable eventCv_;
    std::deque<IpcEvent> events_;
    static const size_t kMaxQueuedEvents = 512;
    uint64_t eventSequence_ = 0;      // monotonic; assigns TargetEvent.sequence
    TargetEvent lastEvent_;           // highest-sequence event (never popped)

    mutable std::mutex errMutex_;
    std::wstring lastError_;

    std::atomic<unsigned long long> nRequests_{0};
    std::atomic<unsigned long long> nTimeouts_{0};
    std::atomic<unsigned long long> nStale_{0};
    std::atomic<unsigned long long> nEvents_{0};
    std::atomic<unsigned long long> nProtocolErrors_{0};

    int requestTimeoutMs_ = bbipc::kRequestTimeoutMs;
};

}  // namespace bbx