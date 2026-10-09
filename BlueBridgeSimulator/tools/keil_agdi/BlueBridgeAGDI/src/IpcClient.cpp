/*
 * BlueBridge Debug IPC client -- implementation (stage 7-2B.2 sections 11-23).
 *
 * Overlapped I/O lifecycle rule (section 17, the stage 7-2A lesson): every
 * OVERLAPPED lives in a stack object whose event is closed only AFTER
 * GetOverlappedResult() confirmed completion (or after CancelIoEx() plus the
 * blocking GetOverlappedResult). CancelIoEx is never followed by a detached
 * stack frame.
 */

#include "IpcClient.h"

#include <process.h>

#include <algorithm>
#include <chrono>

namespace {

struct OverlappedOp {
    OVERLAPPED ov;
    HANDLE ev;

    OverlappedOp() {
        memset(&ov, 0, sizeof(ov));
        ev = CreateEventW(NULL, TRUE, FALSE, NULL);
        ov.hEvent = ev;
    }
    ~OverlappedOp() {
        if (ev != NULL) CloseHandle(ev);
    }
};

std::wstring NormalizePipeName(const std::wstring &in) {
    const wchar_t kPrefix[] = L"\\\\.\\pipe\\";
    if (in.size() >= 9 && _wcsnicmp(in.c_str(), kPrefix, 9) == 0) return in;
    size_t i = 0;
    while (i < in.size() && in[i] == L'\\') ++i;
    return std::wstring(kPrefix) + in.substr(i);
}

std::wstring WinErr(DWORD e) {
    wchar_t *msg = NULL;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, e, 0, (LPWSTR)&msg, 0, NULL);
    std::wstring s;
    if (n != 0 && msg != NULL) {
        s.assign(msg, n);
        while (!s.empty() && (s[s.size() - 1] == L'\r' ||
                              s[s.size() - 1] == L'\n' ||
                              s[s.size() - 1] == L' ')) {
            s.erase(s.size() - 1);
        }
        LocalFree(msg);
    } else {
        wchar_t buf[64];
        _snwprintf_s(buf, _TRUNCATE, L"error %lu", (unsigned long)e);
        s = buf;
    }
    return s;
}

}  // namespace

namespace bbx {

IpcClient::IpcClient() {}

IpcClient::~IpcClient() { disconnect(); }

/* --------------------------------------------------------------------------
 * lifecycle
 * ------------------------------------------------------------------------ */

bool IpcClient::connect(const std::wstring &pipeName, int timeoutMs) {
    if (connected()) return true;

    pipeName_ = NormalizePipeName(pipeName);
    shuttingDown_ = false;
    {
        std::lock_guard<std::mutex> lk(eventMutex_);
        events_.clear();
    }
    Logf("connecting pipe=%ls timeout=%dms", pipeName_.c_str(), timeoutMs);

    const ULONGLONG deadline =
        GetTickCount64() + (ULONGLONG)(timeoutMs > 0 ? timeoutMs : 1);
    for (;;) {
        HANDLE h = CreateFileW(pipeName_.c_str(), GENERIC_READ | GENERIC_WRITE,
                               0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                               NULL);
        if (h != INVALID_HANDLE_VALUE) {
            pipe_ = h;
            break;
        }
        const DWORD e = GetLastError();
        if (e == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(pipeName_.c_str(), 100);
        } else if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            Sleep(50);   // the simulator may still be starting up
        } else {
            const std::wstring msg = L"open pipe failed: " + WinErr(e);
            setLastError(msg);
            Logf("connect failed: %s", ToUtf8(msg.c_str()).c_str());
            return false;
        }
        if (GetTickCount64() >= deadline) {
            setLastError(L"connect timeout waiting for pipe");
            Logf("connect timeout pipe=%ls", pipeName_.c_str());
            return false;
        }
    }

    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(pipe_, &mode, NULL, NULL);

    uintptr_t th = _beginthreadex(NULL, 0, &IpcClient::readerTrampoline, this,
                                  0, NULL);
    if (th == 0) {
        setLastError(L"could not start reader thread");
        Logf("connect failed: could not start reader thread");
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
        return false;
    }
    readerThread_ = (HANDLE)th;
    connected_ = true;
    Logf("connected pipe=%ls", pipeName_.c_str());
    return true;
}

void IpcClient::disconnect() {
    const bool hadThread = readerThread_ != NULL;
    if (!hadThread && pipe_ == INVALID_HANDLE_VALUE) return;

    shuttingDown_ = true;
    if (pipe_ != INVALID_HANDLE_VALUE) CancelIoEx(pipe_, NULL);
    wakeEventWaiters();   // unblock a run waiter before joining the reader
    if (readerThread_ != NULL) {
        WaitForSingleObject(readerThread_, 5000);
        CloseHandle(readerThread_);
        readerThread_ = NULL;
    }
    failAllPending(L"disconnected");
    closePipeHandle();
    connected_ = false;
    if (hadThread) Logf("disconnected pipe=%ls", pipeName_.c_str());
}

bool IpcClient::connected() const {
    return connected_.load() && !shuttingDown_.load();
}

void IpcClient::closePipeHandle() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
}

void IpcClient::setLastError(const std::wstring &text) {
    std::lock_guard<std::mutex> lk(errMutex_);
    lastError_ = text;
}

std::wstring IpcClient::lastErrorCopy() const {
    std::lock_guard<std::mutex> lk(errMutex_);
    return lastError_;
}

/* --------------------------------------------------------------------------
 * reader thread (the ONLY thread that ever calls ReadFile -- section 11/33)
 * ------------------------------------------------------------------------ */

unsigned __stdcall IpcClient::readerTrampoline(void *self) {
    static_cast<IpcClient *>(self)->readerLoop();
    return 0;
}

void IpcClient::readerLoop() {
    std::vector<uint8_t> headerBuf(bbipc::kHeaderSize);

    for (;;) {
        if (shuttingDown_.load()) break;
        if (!readExact(headerBuf.data(), headerBuf.size())) break;

        PacketHeader h;
        decodeHeader(headerBuf.data(), h);
        if (h.magic != bbipc::kMagic) {
            nProtocolErrors_++;
            markConnectionLost(L"bad magic on the wire");
            break;
        }
        if (h.versionMajor != bbipc::kVersionMajor) {
            nProtocolErrors_++;
            Logf("protocol major mismatch: server %u.%u, client %u.%u", h.versionMajor, h.versionMinor, bbipc::kVersionMajor, bbipc::kVersionMinor);
            markConnectionLost(L"protocol major mismatch");
            break;
        }
        if (h.payloadSize > bbipc::kMaxPayload) {
            nProtocolErrors_++;
            markConnectionLost(L"payloadSize above kMaxPayload");
            break;
        }

        std::vector<uint8_t> payload;
        if (h.payloadSize > 0) {
            payload.resize(h.payloadSize);
            if (!readExact(payload.data(), payload.size())) break;
        }

        if (h.kind == bbipc::kPacketResponse) {
            completePending(h.requestId, h.status, payload);
        } else if (h.kind == bbipc::kPacketEvent) {
            IpcEvent ev;
            if (parseEventPayload(payload, h.opcode, ev)) {
                nEvents_++;
                Logf("event op=0x%04X stopReason=%u pc=0x%08lX cycles=%llu", h.opcode,
                     ev.stopReason, (unsigned long)ev.pc, ev.cycles);
                queueEvent(ev);
            } else {
                nProtocolErrors_++;
                Logf("malformed event op=0x%04X (%u bytes)", h.opcode,
                     (unsigned)payload.size());
            }
        } else {
            nProtocolErrors_++;
            markConnectionLost(L"unexpected packet kind");
            break;
        }
    }

    if (!shuttingDown_.load()) markConnectionLost(L"pipe closed by the server");
}

void IpcClient::markConnectionLost(const std::wstring &reason) {
    const bool wasConnected = connected_.exchange(false);
    shuttingDown_ = true;
    if (pipe_ != INVALID_HANDLE_VALUE) CancelIoEx(pipe_, NULL);
    failAllPending(reason);
    wakeEventWaiters();   // a run waiter must never sleep through a dead pipe
    if (wasConnected) Logf("connection lost: %s", ToUtf8(reason.c_str()).c_str());
}

void IpcClient::failAllPending(const std::wstring &reason) {
    std::lock_guard<std::mutex> lk(pendingMutex_);
    if (pending_.empty()) return;
    Logf("failing %u pending request(s): %s", (unsigned)pending_.size(),
         ToUtf8(reason.c_str()).c_str());
    for (auto &kv : pending_) {
        kv.second->status = kStConnectionLost;
        kv.second->done = true;
    }
    pendingCv_.notify_all();
}

void IpcClient::completePending(uint32_t id, uint32_t status,
                                const std::vector<uint8_t> &payload) {
    std::lock_guard<std::mutex> lk(pendingMutex_);
    auto it = pending_.find(id);
    if (it == pending_.end()) {
        nStale_++;
        Logf("stale response id=%u status=%u dropped (request already abandoned)",
             id, status);
        return;
    }
    it->second->status = status;
    it->second->payload = payload;
    it->second->done = true;
    pendingCv_.notify_all();
}

void IpcClient::queueEvent(const IpcEvent &ev) {
    std::lock_guard<std::mutex> lk(eventMutex_);
    events_.push_back(ev);
    while (events_.size() > kMaxQueuedEvents) events_.pop_front();
    /* B.3: assign the local sequence and publish the event to the waiters.
     * The sequence is monotonic for the lifetime of the client; connect()
     * deliberately does NOT reset it (an event from a previous session could
     * otherwise receive a sequence a new run waiter already passed). */
    TargetEvent te;
    te.sequence = ++eventSequence_;
    te.opcode = ev.opcode;
    te.stopReason = ev.stopReason;
    te.pc = ev.pc;
    te.cycles = ev.cycles;
    lastEvent_ = te;
    eventCv_.notify_all();
}

void IpcClient::wakeEventWaiters() { eventCv_.notify_all(); }

uint64_t IpcClient::latestEventSequence() const {
    std::lock_guard<std::mutex> lk(eventMutex_);
    return lastEvent_.sequence;
}

EventWaitResult IpcClient::waitForEventAfter(uint64_t baselineSequence,
                                            TargetEvent *out, int timeoutMs) {
    std::unique_lock<std::mutex> lk(eventMutex_);
    const ULONGLONG deadline =
        timeoutMs > 0 ? GetTickCount64() + (ULONGLONG)timeoutMs : 0;
    for (;;) {
        if (lastEvent_.sequence > baselineSequence) {
            if (out != NULL) *out = lastEvent_;
            return EventWaitResult::Event;
        }
        if (shuttingDown_.load()) return EventWaitResult::Shutdown;
        if (!connected_.load()) return EventWaitResult::ConnectionLost;
        if (deadline != 0) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) return EventWaitResult::Timeout;
            const unsigned long long left = deadline - now;
            eventCv_.wait_for(lk, std::chrono::milliseconds(
                                     (long long)std::min<unsigned long long>(left, 50)));
        } else {
            /* A long/infinite execution wait: the 50 ms slice only re-checks the
             * connection flags -- a real wakeup happens through queueEvent() or
             * wakeEventWaiters() (no busy polling, spec section 22). */
            eventCv_.wait_for(lk, std::chrono::milliseconds(50));
        }
    }
}

IpcClient::StopBaseline IpcClient::captureStopBaseline() {
    StopBaseline b;
    TargetState st;
    if (getState(&st).ok()) {
        b.valid = (st.state != bbipc::kWireStateRunning);
        b.reason = st.stopReason;
        b.pc = st.pc & ~1u;
        b.cycles = st.virtualCycles;
    }
    b.eventSequence = latestEventSequence();
    return b;
}

EventWaitResult IpcClient::waitForRunStop(const StopBaseline &base, int timeoutMs,
                                          TargetEvent *out) {
    const ULONGLONG deadline =
        timeoutMs > 0 ? GetTickCount64() + (ULONGLONG)timeoutMs : 0;
    int repeats = 0;
    for (;;) {
        int slice = 0;
        if (deadline != 0) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) return EventWaitResult::Timeout;
            slice = (int)std::min<unsigned long long>(deadline - now, 60000);
        }
        TargetEvent ev;
        const EventWaitResult wr =
            waitForEventAfter(base.eventSequence, &ev, slice == 0 ? 0 : slice);
        if (wr != EventWaitResult::Event) return wr;

        const bool repeatsPreviousState =
            base.valid && ev.stopReason == base.reason &&
            (ev.pc & ~1u) == base.pc && ev.cycles == base.cycles;
        if (!repeatsPreviousState) {
            if (out != NULL) *out = ev;
            return EventWaitResult::Event;
        }

        /* Leftover event of the previous operation (delivered late). Only
         * believe it when the target really is halted now -- the simulator
         * clears its stop state on RESUME, so a halted target proves a new stop
         * happened (even if it looks identical). */
        ++repeats;
        TargetState st;
        if (getState(&st).ok() && st.state != bbipc::kWireStateRunning) {
            Logf("run-stop: repeated stop event seq=%llu (%lu @0x%08lX cycles=%llu) "
                 "accepted: target is Halted now",
                 (unsigned long long)ev.sequence, (unsigned long)ev.stopReason,
                 (unsigned long)(ev.pc & ~1u), (unsigned long long)ev.cycles);
            if (out != NULL) *out = ev;
            return EventWaitResult::Event;
        }
        Logf("run-stop: stale event seq=%llu (repeats the pre-run stop state) "
             "ignored, still waiting", (unsigned long long)ev.sequence);
    }
}

bool IpcClient::popEvent(IpcEvent &ev) {
    std::lock_guard<std::mutex> lk(eventMutex_);
    if (events_.empty()) return false;
    ev = events_.front();
    events_.pop_front();
    return true;
}

size_t IpcClient::queuedEvents() const {
    std::lock_guard<std::mutex> lk(eventMutex_);
    return events_.size();
}

IpcClient::Stats IpcClient::stats() const {
    Stats s;
    s.requests = nRequests_.load();
    s.timeouts = nTimeouts_.load();
    s.staleResponses = nStale_.load();
    s.events = nEvents_.load();
    s.protocolErrors = nProtocolErrors_.load();
    return s;
}

/* --------------------------------------------------------------------------
 * exact, overlapped, cancellable I/O
 * ------------------------------------------------------------------------ */

bool IpcClient::readExact(uint8_t *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        if (shuttingDown_.load()) return false;

        OverlappedOp op;
        if (op.ev == NULL) return false;

        if (!ReadFile(pipe_, buf + got, (DWORD)(n - got), NULL, &op.ov)) {
            const DWORD e = GetLastError();
            if (e != ERROR_IO_PENDING) {
                if (!shuttingDown_.load()) {
                    setLastError(L"read failed: " + WinErr(e));
                    Logf("read failed: %s", ToUtf8(WinErr(e).c_str()).c_str());
                }
                return false;
            }
        }
        for (;;) {
            const DWORD w = WaitForSingleObject(op.ev, 100);
            if (w == WAIT_OBJECT_0) break;
            if (shuttingDown_.load()) CancelIoEx(pipe_, &op.ov);
        }
        DWORD bytes = 0;
        if (!GetOverlappedResult(pipe_, &op.ov, &bytes, TRUE)) {
            const DWORD e = GetLastError();
            if (!shuttingDown_.load()) {
                setLastError(L"read failed: " + WinErr(e));
                Logf("read failed: %s", ToUtf8(WinErr(e).c_str()).c_str());
            }
            return false;
        }
        if (bytes == 0) return false;   // server closed the pipe
        got += bytes;
    }
    return true;
}

bool IpcClient::writeAll(const uint8_t *buf, size_t n, int timeoutMs) {
    const ULONGLONG deadline =
        GetTickCount64() + (ULONGLONG)(timeoutMs > 0 ? timeoutMs : 1);
    size_t sent = 0;
    while (sent < n) {
        if (shuttingDown_.load()) return false;

        OverlappedOp op;
        if (op.ev == NULL) return false;

        if (!WriteFile(pipe_, buf + sent, (DWORD)(n - sent), NULL, &op.ov)) {
            const DWORD e = GetLastError();
            if (e != ERROR_IO_PENDING) {
                setLastError(L"write failed: " + WinErr(e));
                Logf("write failed: %s", ToUtf8(WinErr(e).c_str()).c_str());
                return false;
            }
        }
        bool timedOut = false;
        for (;;) {
            const DWORD w = WaitForSingleObject(op.ev, 50);
            if (w == WAIT_OBJECT_0) break;
            if (shuttingDown_.load()) {
                CancelIoEx(pipe_, &op.ov);
                break;
            }
            if (GetTickCount64() >= deadline) {
                timedOut = true;
                CancelIoEx(pipe_, &op.ov);
                break;
            }
        }
        DWORD bytes = 0;
        if (!GetOverlappedResult(pipe_, &op.ov, &bytes, TRUE)) {
            const DWORD e = GetLastError();
            if (!shuttingDown_.load()) {
                if (timedOut) setLastError(L"write timeout");
                else setLastError(L"write failed: " + WinErr(e));
                Logf("write failed (%s): %s",
                     ToUtf8(timedOut ? L"timeout" : L"io").c_str(),
                     ToUtf8(timedOut ? L"deadline exceeded" : WinErr(e).c_str()).c_str());
            }
            return false;
        }
        if (bytes == 0) return false;
        sent += bytes;
    }
    return true;
}

/* --------------------------------------------------------------------------
 * request core
 * ------------------------------------------------------------------------ */

uint32_t IpcClient::allocRequestId() {
    for (;;) {
        const uint32_t id = nextRequestId_.fetch_add(1);
        if (id == 0) continue;   // 0 is reserved for events
        std::lock_guard<std::mutex> lk(pendingMutex_);
        if (pending_.find(id) == pending_.end()) return id;
    }
}

RequestResult IpcClient::request(uint16_t opcode,
                                 const std::vector<uint8_t> &payload,
                                 int timeoutMs) {
    RequestResult res;
    if (!connected()) {
        res.status = kStNotConnected;
        return res;
    }
    if (payload.size() > bbipc::kMaxPayload) {
        res.status = kStBadArgument;
        return res;
    }

    const uint32_t id = allocRequestId();
    auto p = std::make_shared<Pending>();
    p->id = id;
    {
        std::lock_guard<std::mutex> lk(pendingMutex_);
        pending_[id] = p;
    }

    PacketHeader h;
    h.kind = bbipc::kPacketRequest;
    h.opcode = opcode;
    h.requestId = id;
    h.payloadSize = (uint32_t)payload.size();
    std::vector<uint8_t> frame = encodeHeader(h);
    frame.insert(frame.end(), payload.begin(), payload.end());

    {
        std::lock_guard<std::mutex> wl(writeMutex_);
        if (!writeAll(frame.data(), frame.size(), timeoutMs)) {
            {
                std::lock_guard<std::mutex> lk(pendingMutex_);
                pending_.erase(id);
            }
            // A write that failed (or could not be flushed in time) leaves the
            // byte stream in an unknown state: the session is over (section 15).
            markConnectionLost(L"request write failed");
            res.status = kStConnectionLost;
            return res;
        }
    }
    nRequests_++;

    const ULONGLONG deadline =
        GetTickCount64() + (ULONGLONG)(timeoutMs > 0 ? timeoutMs : 1);
    std::unique_lock<std::mutex> lk(pendingMutex_);
    for (;;) {
        if (p->done) break;
        if (!connected_.load() || shuttingDown_.load()) break;
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        const unsigned long long left = deadline - now;
        pendingCv_.wait_for(
            lk, std::chrono::milliseconds(
                    (long long)std::min<unsigned long long>(left, 25)));
    }

    if (p->done) {
        res.status = p->status;
        res.payload.swap(p->payload);
        pending_.erase(id);
        return res;
    }

    pending_.erase(id);   // abandoned: a late response is dropped, never
                          // matched onto a newer request (section 21)
    if (!connected_.load() || shuttingDown_.load()) {
        res.status = kStConnectionLost;
        return res;
    }
    nTimeouts_++;
    Logf("timeout id=%u op=0x%04X after %d ms", id, opcode, timeoutMs);
    res.status = bbipc::kStTimeout;
    return res;
}

/* --------------------------------------------------------------------------
 * request API
 * ------------------------------------------------------------------------ */

RequestResult IpcClient::hello(uint32_t clientType, uint32_t clientPid) {
    return request(bbipc::kOpHello,
                   buildHello(bbipc::kVersionMajor, bbipc::kVersionMinor,
                              clientPid, clientType),
                   requestTimeoutMs_);
}

RequestResult IpcClient::ping(uint64_t cookie) {
    return request(bbipc::kOpPing, buildU64(cookie), requestTimeoutMs_);
}

RequestResult IpcClient::getCapabilities(TargetCaps *out) {
    RequestResult r =
        request(bbipc::kOpGetCapabilities, std::vector<uint8_t>(), requestTimeoutMs_);
    if (r.ok() && out != NULL && !parseCapabilities(r.payload, *out)) {
        r.status = kStBadResponse;
        Logf("GET_CAPABILITIES payload did not parse");
    }
    return r;
}

RequestResult IpcClient::getState(TargetState *out) {
    RequestResult r =
        request(bbipc::kOpGetState, std::vector<uint8_t>(), requestTimeoutMs_);
    if (r.ok() && out != NULL && !parseState(r.payload, *out)) {
        r.status = kStBadResponse;
        Logf("GET_STATE payload did not parse");
    }
    return r;
}

RequestResult IpcClient::getStopInfo(uint32_t *reason, uint32_t *pc) {
    RequestResult r =
        request(bbipc::kOpGetStopInfo, std::vector<uint8_t>(), requestTimeoutMs_);
    if (r.ok() && !parseStopInfo(r.payload, *reason, *pc)) {
        r.status = kStBadResponse;
        Logf("GET_STOP_INFO payload did not parse");
    }
    return r;
}

RequestResult IpcClient::readRegister(uint32_t id, uint64_t *value) {
    RequestResult r =
        request(bbipc::kOpReadRegister, buildU32(id), requestTimeoutMs_);
    if (r.ok() && !parseU64(r.payload, *value)) {
        r.status = kStBadResponse;
        Logf("READ_REGISTER payload did not parse");
    }
    return r;
}

RequestResult IpcClient::writeRegister(uint32_t id, uint64_t value) {
    return request(bbipc::kOpWriteRegister, buildWriteRegister(id, value),
                   requestTimeoutMs_);
}

RequestResult IpcClient::readRegisters(const std::vector<uint32_t> &ids,
                                       std::vector<RegisterValue> *out) {
    RequestResult r = request(bbipc::kOpReadRegisters, buildReadRegisters(ids),
                              requestTimeoutMs_);
    if (r.ok() && out != NULL && !parseRegisterBatch(r.payload, *out)) {
        r.status = kStBadResponse;
        Logf("READ_REGISTERS payload did not parse");
    }
    return r;
}

RequestResult IpcClient::writeRegisters(const std::vector<RegisterValue> &in,
                                        std::vector<RegisterValue> *out) {
    RequestResult r = request(bbipc::kOpWriteRegisters, buildWriteRegisters(in),
                              requestTimeoutMs_);
    if (r.ok() && out != NULL && !parseWriteRegisterBatch(r.payload, *out)) {
        r.status = kStBadResponse;
        Logf("WRITE_REGISTERS payload did not parse");
    }
    return r;
}

RequestResult IpcClient::readMemory(uint32_t address, uint32_t length,
                                    std::vector<uint8_t> *out) {
    if (length > bbipc::kMaxMemoryTransfer) {
        RequestResult r;
        r.status = kStBadArgument;
        return r;
    }
    RequestResult r = request(bbipc::kOpReadMemory,
                              buildReadMemory(address, length), requestTimeoutMs_);
    if (r.ok()) {
        if (r.payload.size() != length) {
            r.status = kStBadResponse;
            Logf("READ_MEMORY returned %u bytes for a %u byte request",
                 (unsigned)r.payload.size(), length);
        } else if (out != NULL) {
            *out = r.payload;
        }
    }
    return r;
}

RequestResult IpcClient::writeMemory(uint32_t address, const uint8_t *data,
                                     size_t length) {
    if (length > bbipc::kMaxMemoryTransfer) {
        RequestResult r;
        r.status = kStBadArgument;
        return r;
    }
    return request(bbipc::kOpWriteMemory,
                   buildWriteMemory(address, data, length), requestTimeoutMs_);
}

RequestResult IpcClient::resetHalt() {
    return request(bbipc::kOpResetHalt, std::vector<uint8_t>(),
                   requestTimeoutMs_);
}

RequestResult IpcClient::resetRun() {
    return request(bbipc::kOpResetRun, std::vector<uint8_t>(),
                   requestTimeoutMs_);
}

RequestResult IpcClient::step() {
    return request(bbipc::kOpStep, std::vector<uint8_t>(), requestTimeoutMs_);
}

RequestResult IpcClient::halt() {
    return request(bbipc::kOpHalt, std::vector<uint8_t>(), requestTimeoutMs_);
}

RequestResult IpcClient::resume() {
    return request(bbipc::kOpResume, std::vector<uint8_t>(), requestTimeoutMs_);
}

RequestResult IpcClient::addBreakpoint(uint32_t address) {
    return request(bbipc::kOpAddBreakpoint, buildU32(address),
                   requestTimeoutMs_);
}

RequestResult IpcClient::removeBreakpoint(uint32_t address) {
    return request(bbipc::kOpRemoveBreakpoint, buildU32(address),
                   requestTimeoutMs_);
}

RequestResult IpcClient::clearBreakpoints() {
    return request(bbipc::kOpClearBreakpoints, std::vector<uint8_t>(),
                   requestTimeoutMs_);
}

RequestResult IpcClient::programBegin(uint64_t *token, uint32_t *flashBase,
                                      uint32_t *flashSize) {
    RequestResult r =
        request(bbipc::kOpProgramBegin, std::vector<uint8_t>(), requestTimeoutMs_);
    if (r.ok() && !parseProgramBegin(r.payload, *token, *flashBase, *flashSize)) {
        r.status = kStBadResponse;
        Logf("PROGRAM_BEGIN payload did not parse");
    }
    return r;
}

RequestResult IpcClient::programErase(uint64_t token, uint32_t address,
                                      uint32_t size) {
    return request(bbipc::kOpProgramErase, buildProgramErase(token, address, size),
                   bbipc::kRequestTimeoutMs);
}

RequestResult IpcClient::programWrite(uint64_t token, uint32_t address,
                                      const uint8_t *data, size_t length) {
    if (length > bbipc::kMaxProgramTransfer) {
        RequestResult r;
        r.status = kStBadArgument;
        return r;
    }
    return request(bbipc::kOpProgramWrite,
                   buildProgramWrite(token, address, data, length),
                   bbipc::kRequestTimeoutMs);
}

RequestResult IpcClient::programEnd(uint64_t token, uint32_t *initialSp,
                                    uint32_t *resetPc) {
    RequestResult r = request(bbipc::kOpProgramEnd, buildU64(token),
                              bbipc::kProgramEndTimeoutMs);
    if (r.ok() && !parseProgramEnd(r.payload, *initialSp, *resetPc)) {
        r.status = kStBadResponse;
        Logf("PROGRAM_END payload did not parse");
    }
    return r;
}

RequestResult IpcClient::programAbort(uint64_t token) {
    return request(bbipc::kOpProgramAbort, buildU64(token), requestTimeoutMs_);
}

}  // namespace bbx