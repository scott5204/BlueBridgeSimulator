#include "debug/ipc/DebugIpcServer.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "debug/IDebugTarget.h"
#include "debug/IFlashProgrammer.h"
#include "debug/ipc/DebugIpcCodec.h"

namespace {

// Internal opcode (never on the wire): session cleanup, executed by the owner
// thread when a client disconnects -- the pipe worker must never halt the
// target or clear breakpoints itself (spec 158/159).
constexpr uint16_t kOpInternalSessionCleanup = 0x7FFEu;

// Owner-pump budget (spec 111): never drain the queue without a limit.
constexpr int kMaxCommandsPerPump = 64;
constexpr long long kPumpBudgetMs = 2;

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return std::string(buf);
}

// ---- the ONE DebugStatus -> WireStatus conversion (spec 16) ---------------
bbipc::WireStatus wireStatusOf(DebugStatus s) {
    switch (s) {
    case DebugStatus::Ok: return bbipc::kStOk;
    case DebugStatus::InvalidState: return bbipc::kStInvalidState;
    case DebugStatus::InvalidRegister: return bbipc::kStInvalidRegister;
    case DebugStatus::InvalidAddress: return bbipc::kStInvalidAddress;
    case DebugStatus::Unsupported: return bbipc::kStUnsupported;
    case DebugStatus::CpuError: return bbipc::kStInternalError;
    case DebugStatus::ProgramNotActive: return bbipc::kStProgramNotActive;
    case DebugStatus::ProgramAlreadyActive:
        return bbipc::kStProgramAlreadyActive;
    case DebugStatus::ProgramTokenInvalid: return bbipc::kStProgramTokenInvalid;
    case DebugStatus::ProgramRangeInvalid: return bbipc::kStProgramRangeInvalid;
    }
    return bbipc::kStInternalError;
}

bbipc::WireTargetState wireStateOf(TargetState s) {
    switch (s) {
    case TargetState::Halted: return bbipc::kWireStateHalted;
    case TargetState::Running: return bbipc::kWireStateRunning;
    case TargetState::Reset: return bbipc::kWireStateReset;
    case TargetState::Fault: return bbipc::kWireStateFault;
    }
    return bbipc::kWireStateHalted;
}

bbipc::WireStopReason wireReasonOf(StopReason r) {
    switch (r) {
    case StopReason::None: return bbipc::kWireStopNone;
    case StopReason::UserHalt: return bbipc::kWireStopUserHalt;
    case StopReason::Breakpoint: return bbipc::kWireStopBreakpoint;
    case StopReason::SingleStep: return bbipc::kWireStopSingleStep;
    case StopReason::Reset: return bbipc::kWireStopReset;
    case StopReason::Fault: return bbipc::kWireStopFault;
    }
    return bbipc::kWireStopNone;
}

const char* wireStatusName(bbipc::WireStatus s) {
    switch (s) {
    case bbipc::kStOk: return "Ok";
    case bbipc::kStInvalidCommand: return "InvalidCommand";
    case bbipc::kStInvalidState: return "InvalidState";
    case bbipc::kStInvalidRegister: return "InvalidRegister";
    case bbipc::kStInvalidAddress: return "InvalidAddress";
    case bbipc::kStUnsupported: return "Unsupported";
    case bbipc::kStBusy: return "Busy";
    case bbipc::kStTimeout: return "Timeout";
    case bbipc::kStProtocolError: return "ProtocolError";
    case bbipc::kStTargetFault: return "TargetFault";
    case bbipc::kStProgramNotActive: return "ProgramNotActive";
    case bbipc::kStProgramAlreadyActive: return "ProgramAlreadyActive";
    case bbipc::kStProgramTokenInvalid: return "ProgramTokenInvalid";
    case bbipc::kStProgramRangeInvalid: return "ProgramRangeInvalid";
    case bbipc::kStInternalError: return "InternalError";
    }
    return "?";
}

// Wire register id -> DebugRegister. Frozen mapping (spec 21/22).
bool wireRegisterOf(uint32_t id, DebugRegister& out) {
    if (id <= 12) {
        out = static_cast<DebugRegister>(int(DebugRegister::R0) + int(id));
        return true;
    }
    if (id >= bbipc::kWireRegS0 && id <= bbipc::kWireRegS31) {
        const int off = int(id) - int(bbipc::kWireRegS0);
        out = static_cast<DebugRegister>(int(DebugRegister::S0) + off);
        return true;
    }
    switch (id) {
    case bbipc::kWireRegSP: out = DebugRegister::SP; return true;
    case bbipc::kWireRegLR: out = DebugRegister::LR; return true;
    case bbipc::kWireRegPC: out = DebugRegister::PC; return true;
    case bbipc::kWireRegXPSR: out = DebugRegister::XPSR; return true;
    case bbipc::kWireRegMSP: out = DebugRegister::MSP; return true;
    case bbipc::kWireRegPSP: out = DebugRegister::PSP; return true;
    case bbipc::kWireRegPRIMASK: out = DebugRegister::PRIMASK; return true;
    case bbipc::kWireRegBASEPRI: out = DebugRegister::BASEPRI; return true;
    case bbipc::kWireRegFAULTMASK: out = DebugRegister::FAULTMASK; return true;
    case bbipc::kWireRegCONTROL: out = DebugRegister::CONTROL; return true;
    case bbipc::kWireRegFPSCR: out = DebugRegister::FPSCR; return true;
    default: return false;
    }
}

bool isKnownOpcode(uint16_t op) {
    switch (op) {
    case bbipc::kOpHello:
    case bbipc::kOpGetCapabilities:
    case bbipc::kOpGetState:
    case bbipc::kOpGetStopInfo:
    case bbipc::kOpHalt:
    case bbipc::kOpResume:
    case bbipc::kOpStep:
    case bbipc::kOpResetHalt:
    case bbipc::kOpResetRun:
    case bbipc::kOpPing:
    case bbipc::kOpReadRegister:
    case bbipc::kOpWriteRegister:
    case bbipc::kOpReadRegisters:
    case bbipc::kOpWriteRegisters:
    case bbipc::kOpReadMemory:
    case bbipc::kOpWriteMemory:
    case bbipc::kOpAddBreakpoint:
    case bbipc::kOpRemoveBreakpoint:
    case bbipc::kOpClearBreakpoints:
    case bbipc::kOpProgramBegin:
    case bbipc::kOpProgramErase:
    case bbipc::kOpProgramWrite:
    case bbipc::kOpProgramEnd:
    case bbipc::kOpProgramAbort:
        return true;
    default:
        return false;
    }
}

std::string normalizePipeName(const std::string& in) {
    if (in.empty()) return in;
    if (in.rfind("\\\\", 0) == 0 || in.rfind("//", 0) == 0) return in;
    return "\\\\.\\pipe\\" + in;
}

// ---- decoded-argument contract (worker thread) ----------------------------
// opcode                     u32a            u32b        u64a     ids/bytes
// HELLO                      versionPacked   clientPid   clientType
// READ_REGISTER              regId
// WRITE_REGISTER             regId                       value
// READ_REGISTERS             count                        ids[]
// WRITE_REGISTERS            count                        ids[] + bytes (u64 LE)
// READ_MEMORY                address         length
// WRITE_MEMORY               address         length              bytes
// ADD/REMOVE_BREAKPOINT      address
// PROGRAM_BEGIN              flags
// PROGRAM_ERASE              address         size       token
// PROGRAM_WRITE              address         length     token    bytes
// PROGRAM_END / ABORT                                        token
bool decodeRequest(uint16_t opcode, const std::vector<uint8_t>& payload,
                   DebugCommandArgs& args, bbipc::WireStatus& err) {
    bbipc::PayloadReader r(payload.data(), payload.size());
    const uint32_t kMaxRegisterBatch = 128;
    switch (opcode) {
    case bbipc::kOpHello: {
        uint16_t maj = 0, min = 0;
        uint32_t pid = 0, type = 0;
        r.u16(maj);
        r.u16(min);
        r.u32(pid);
        r.u32(type);
        args.u32a = (uint32_t(maj) << 16) | uint32_t(min);
        args.u32b = pid;
        args.u64a = type;
        break;
    }
    case bbipc::kOpGetCapabilities:
    case bbipc::kOpGetState:
    case bbipc::kOpGetStopInfo:
    case bbipc::kOpHalt:
    case bbipc::kOpResume:
    case bbipc::kOpStep:
    case bbipc::kOpResetHalt:
    case bbipc::kOpResetRun:
    case bbipc::kOpClearBreakpoints:
        break;  // empty payload
    case bbipc::kOpReadRegister:
    case bbipc::kOpAddBreakpoint:
    case bbipc::kOpRemoveBreakpoint:
        r.u32(args.u32a);
        break;
    case bbipc::kOpWriteRegister:
        r.u32(args.u32a);
        r.u64(args.u64a);
        break;
    case bbipc::kOpReadRegisters: {
        uint32_t count = 0;
        r.u32(count);
        if (count > kMaxRegisterBatch) { err = bbipc::kStProtocolError; return false; }
        args.u32a = count;
        args.ids.resize(count);
        for (uint32_t i = 0; i < count; i++) r.u32(args.ids[i]);
        break;
    }
    case bbipc::kOpWriteRegisters: {
        uint32_t count = 0;
        r.u32(count);
        if (count > kMaxRegisterBatch) { err = bbipc::kStProtocolError; return false; }
        args.u32a = count;
        args.ids.resize(count);
        args.bytes.resize(size_t(count) * 8);
        for (uint32_t i = 0; i < count; i++) {
            r.u32(args.ids[i]);
            r.bytes(args.bytes.data() + size_t(i) * 8, 8);
        }
        break;
    }
    case bbipc::kOpReadMemory:
    case bbipc::kOpWriteMemory: {
        uint32_t len = 0;
        r.u32(args.u32a);
        r.u32(len);
        args.u32b = len;
        if (len > bbipc::kMaxMemoryTransfer) {
            err = bbipc::kStProtocolError;
            return false;
        }
        if (opcode == bbipc::kOpWriteMemory) {
            if (r.remaining() != len) { err = bbipc::kStProtocolError; return false; }
            args.bytes.resize(len);
            r.bytes(args.bytes.data(), len);
        }
        break;
    }
    case bbipc::kOpProgramBegin:
        if (r.remaining() >= 4) r.u32(args.u32a);  // flags (optional)
        break;
    case bbipc::kOpProgramErase:
        r.u64(args.u64a);
        r.u32(args.u32a);
        r.u32(args.u32b);
        break;
    case bbipc::kOpProgramWrite: {
        uint32_t len = 0;
        r.u64(args.u64a);
        r.u32(args.u32a);
        r.u32(len);
        if (len > bbipc::kMaxProgramTransfer) {
            err = bbipc::kStProtocolError;
            return false;
        }
        if (r.remaining() != len) { err = bbipc::kStProtocolError; return false; }
        args.bytes.resize(len);
        r.bytes(args.bytes.data(), len);
        break;
    }
    case bbipc::kOpProgramEnd:
    case bbipc::kOpProgramAbort:
        r.u64(args.u64a);
        break;
    default:
        err = bbipc::kStInvalidCommand;
        return false;
    }
    // strict framing: no trailing garbage, no truncated fields (spec 143)
    if (!r.ok() || !r.atEnd()) {
        err = bbipc::kStProtocolError;
        return false;
    }
    return true;
}

struct CommandOutcome {
    bbipc::WireStatus status = bbipc::kStOk;
    std::vector<uint8_t> payload;
};

void writeString(bbipc::PayloadWriter& w, const char* s) {
    const size_t n = s ? std::strlen(s) : 0;
    w.u16(uint16_t(n));
    w.bytes(s, n);
}

}  // namespace

// ===========================================================================
// Impl: pipe worker + shared state
// ===========================================================================
struct DebugIpcServer::Impl {
    Options opt;
    std::string pipeNameFull;

    HANDLE stopEvent = nullptr;   // manual-reset: shutdown requested
    HANDLE eventAvail = nullptr;  // auto-reset: stop events queued
    HANDLE readEvent = nullptr;   // auto-reset: READ completion (dedicated!)
    HANDLE ovEvent = nullptr;     // auto-reset: write / connect completion
    HANDLE pipe = INVALID_HANDLE_VALUE;

    std::thread thread;
    std::atomic<bool> shutdown{false};
    std::atomic<bool> started{false};
    std::atomic<bool> failed{false};

    DebugCommandQueue queue;

    std::function<void(const std::string&)> logSink;
    std::function<void(bool)> attachObserver;

    mutable std::mutex logMutex;
    std::vector<std::string> logLines;

    struct DebugEvent {
        uint16_t opcode = 0;
        uint32_t reason = 0;
        uint32_t pc = 0;
        uint64_t cycles = 0;
    };
    mutable std::mutex eventMutex;
    std::deque<std::pair<uint64_t, DebugEvent>> events;  // (session epoch, event)

    std::atomic<uint64_t> epoch{0};
    std::atomic<bool> clientConnected{false};
    std::atomic<bool> handshaken{false};
    std::atomic<uint64_t> sessionId{0};
    std::atomic<uint64_t> sessionsServed{0};

    uint32_t serverPid = 0;
    uint64_t sessionCounter = 0;
    uint32_t clientPid = 0;
    uint32_t clientType = 0;

    // ---- logging ----------------------------------------------------------
    void log(const std::string& s) {
        std::lock_guard<std::mutex> lk(logMutex);
        logLines.push_back(s);
    }

    void traceLine(const char* dir, uint16_t opcode, uint32_t requestId,
                   size_t size, bbipc::WireStatus status) {
        if (!opt.trace) return;
        log(fmt("[ipc] %s op=0x%04X req=%u len=%u st=%s", dir, opcode,
                requestId, unsigned(size), wireStatusName(status)));
    }

    // ---- overlapped pipe I/O ---------------------------------------------
    enum class IoResult { Ok, Stopped, Broken };

    // Reads exactly @size bytes. While waiting it wakes up on queued stop
    // events (and forwards them) and on shutdown. Partial reads are looped --
    // a Named Pipe is byte mode, a single ReadFile may return less (spec 145).
    //
    // The read uses its OWN completion event (readEvent): writes (responses /
    // events) also use overlapped I/O, and sharing one event let a completed
    // WRITE wake this wait loop, where GetOverlappedResult then returned
    // ERROR_IO_INCOMPLETE -- misinterpreted as a broken pipe, which tore the
    // session down right after the first stop event was forwarded.
    IoResult readExact(void* dst, size_t size, bool forwardEvents) {
        size_t got = 0;
        uint8_t* out = static_cast<uint8_t*>(dst);
        while (got < size) {
            OVERLAPPED ov;
            std::memset(&ov, 0, sizeof(ov));
            ov.hEvent = readEvent;
            ResetEvent(readEvent);
            const DWORD want = DWORD(size - got);
            const BOOL ok = ReadFile(pipe, out + got, want, nullptr, &ov);
            DWORD done = 0;
            if (!ok) {
                const DWORD err = GetLastError();
                if (err != ERROR_IO_PENDING) return ioError(err);
                for (;;) {
                    HANDLE hs[3] = {stopEvent, eventAvail, readEvent};
                    const DWORD w = WaitForMultipleObjects(3, hs, FALSE, 20);
                    if (w == WAIT_OBJECT_0) {
                        // shutdown: cancel and REAP before this stack-local
                        // OVERLAPPED goes out of scope
                        CancelIoEx(pipe, &ov);
                        GetOverlappedResult(pipe, &ov, &done, TRUE);
                        return IoResult::Stopped;
                    }
                    if (w == WAIT_OBJECT_0 + 2) {
                        if (GetOverlappedResult(pipe, &ov, &done, FALSE)) break;
                        if (GetLastError() == ERROR_IO_INCOMPLETE) continue;
                        return ioError(GetLastError());
                    }
                    // event-available / timeout: forward pending events now so
                    // a breakpoint hit reaches the client without waiting for
                    // the next command
                    if (forwardEvents && !drainEvents()) return IoResult::Broken;
                }
            } else {
                done = DWORD(ov.InternalHigh);  // sync completion (bytes in ov)
            }
            if (done == 0) return IoResult::Broken;  // client closed mid-packet
            got += done;
        }
        return IoResult::Ok;
    }

    IoResult writeExact(const void* src, size_t size) {
        const uint8_t* in = static_cast<const uint8_t*>(src);
        size_t sent = 0;
        while (sent < size) {
            OVERLAPPED ov;
            std::memset(&ov, 0, sizeof(ov));
            ov.hEvent = ovEvent;
            ResetEvent(ovEvent);
            const DWORD want = DWORD(size - sent);
            const BOOL ok = WriteFile(pipe, in + sent, want, nullptr, &ov);
            DWORD done = 0;
            if (!ok) {
                const DWORD err = GetLastError();
                if (err != ERROR_IO_PENDING) return ioError(err);
                HANDLE hs[2] = {stopEvent, ovEvent};
                if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) ==
                    WAIT_OBJECT_0) {
                    CancelIoEx(pipe, &ov);
                    GetOverlappedResult(pipe, &ov, &done, TRUE);  // reap
                    return IoResult::Stopped;
                }
                if (!GetOverlappedResult(pipe, &ov, &done, FALSE)) {
                    return ioError(GetLastError());
                }
            } else {
                done = DWORD(ov.InternalHigh);
            }
            if (done == 0) return IoResult::Broken;
            sent += done;
        }
        return IoResult::Ok;
    }

    IoResult ioError(DWORD err) {
        if (err == ERROR_OPERATION_ABORTED) return IoResult::Stopped;
        // ERROR_BROKEN_PIPE / ERROR_PIPE_NOT_CONNECTED / ERROR_NO_DATA: the
        // normal "client went away" path (spec 44) -- never a crash.
        return IoResult::Broken;
    }

    bool writePacket(uint16_t kind, uint16_t opcode, uint32_t requestId,
                     bbipc::WireStatus status,
                     const std::vector<uint8_t>& payload) {
        if (pipe == INVALID_HANDLE_VALUE) return false;
        bbipc::PacketHeader h;
        h.packetKind = kind;
        h.opcode = opcode;
        h.requestId = requestId;
        h.payloadSize = uint32_t(payload.size());
        h.status = uint32_t(status);
        uint8_t raw[bbipc::kHeaderSize];
        bbipc::encodeHeader(raw, h);
        if (writeExact(raw, sizeof(raw)) != IoResult::Ok) return false;
        if (!payload.empty() &&
            writeExact(payload.data(), payload.size()) != IoResult::Ok) {
            return false;
        }
        return true;
    }

    void respond(uint32_t requestId, uint16_t opcode, bbipc::WireStatus status,
                 const std::vector<uint8_t>& payload = {}) {
        writePacket(bbipc::kPacketResponse, opcode, requestId, status, payload);
        traceLine("tx", opcode, requestId, payload.size(), status);
    }

    // ---- async events -----------------------------------------------------
    bool drainEvents() {
        std::deque<std::pair<uint64_t, DebugEvent>> pending;
        {
            std::lock_guard<std::mutex> lk(eventMutex);
            pending.swap(events);
        }
        const uint64_t ep = epoch.load();
        for (auto& it : pending) {
            if (it.first != ep) continue;  // stale session: never send (spec 45)
            const DebugEvent& ev = it.second;
            std::vector<uint8_t> out;
            bbipc::PayloadWriter w(out);
            w.u32(ev.reason);
            w.u32(ev.pc);
            w.u64(ev.cycles);
            if (!writePacket(bbipc::kPacketEvent, ev.opcode, 0, bbipc::kStOk,
                             out)) {
                return false;
            }
            traceLine("evt", ev.opcode, 0, out.size(), bbipc::kStOk);
        }
        return true;
    }

    // ---- pipe lifecycle ---------------------------------------------------
    bool createPipeInstance() {
        // ASCII pipe names only (a debugger session name is pid + nonce).
        std::wstring wide;
        const int n = MultiByteToWideChar(CP_UTF8, 0, pipeNameFull.c_str(), -1,
                                          nullptr, 0);
        if (n > 0) {
            wide.resize(size_t(n));
            MultiByteToWideChar(CP_UTF8, 0, pipeNameFull.c_str(), -1,
                                wide.data(), n);
            wide.resize(size_t(n) - 1);  // drop the terminating NUL
        }
        pipe = CreateNamedPipeW(
            wide.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            // single instance: a second simultaneous debugger gets
            // ERROR_PIPE_BUSY from CreateFile -- "Busy" as specified (spec 46)
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                PIPE_REJECT_REMOTE_CLIENTS,
            1, 64 * 1024, 64 * 1024, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            log(fmt("[ipc] Failed to create pipe '%s' (win32 err=%lu) -- debug "
                    "IPC disabled, the simulator keeps working",
                    pipeNameFull.c_str(), GetLastError()));
            return false;
        }
        return true;
    }

    void closePipe() {
        if (pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
    }

    enum class ConnectResult { Connected, Stopped, Error };

    ConnectResult waitForClient() {
        OVERLAPPED ov;
        std::memset(&ov, 0, sizeof(ov));
        ov.hEvent = ovEvent;
        ResetEvent(ovEvent);
        const BOOL ok = ConnectNamedPipe(pipe, &ov);
        if (!ok) {
            const DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) return ConnectResult::Connected;
            if (err != ERROR_IO_PENDING) return ConnectResult::Error;
            HANDLE hs[2] = {stopEvent, ovEvent};
            if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) ==
                WAIT_OBJECT_0) {
                CancelIoEx(pipe, &ov);
                DWORD reaped = 0;
                GetOverlappedResult(pipe, &ov, &reaped, TRUE);  // reap
                return ConnectResult::Stopped;
            }
            DWORD done = 0;
            if (!GetOverlappedResult(pipe, &ov, &done, FALSE)) {
                return ioError(GetLastError()) == IoResult::Stopped
                           ? ConnectResult::Stopped
                           : ConnectResult::Error;
            }
        }
        return ConnectResult::Connected;
    }

    uint64_t makeSessionId() {
        ++sessionCounter;
        uint64_t id = uint64_t(GetTickCount64()) ^
                      (uint64_t(serverPid) << 32) ^
                      (sessionCounter * 0x9E3779B97F4A7C15ull);
        id &= 0xFFFFFFFFull;
        if (id == 0) id = 1;
        return id;
    }

    // ---- session ----------------------------------------------------------
    // Returns false when the session must be closed (fatal protocol error).
    bool handleRequest(const bbipc::PacketHeader& h,
                       const std::vector<uint8_t>& payload);

    void serveSession() {
        uint8_t header[bbipc::kHeaderSize];
        for (;;) {
            if (shutdown.load()) return;
            if (readExact(header, sizeof(header), true) != IoResult::Ok) return;
            bbipc::PacketHeader h;
            if (!bbipc::decodeHeader(header, h)) {
                log("[ipc] protocol error: bad magic/header -- disconnecting");
                respond(0, 0, bbipc::kStProtocolError);
                return;
            }
            if (h.payloadSize > bbipc::kMaxPayload) {
                log(fmt("[ipc] protocol error: payloadSize %u exceeds the %u "
                        "byte limit -- disconnecting",
                        h.payloadSize, bbipc::kMaxPayload));
                respond(h.requestId, h.opcode, bbipc::kStProtocolError);
                return;
            }
            std::vector<uint8_t> payload;
            if (h.payloadSize) {
                payload.resize(h.payloadSize);
                if (readExact(payload.data(), payload.size(), false) !=
                    IoResult::Ok) {
                    return;
                }
            }
            if (h.packetKind != bbipc::kPacketRequest) {
                respond(h.requestId, h.opcode, bbipc::kStProtocolError);
                continue;
            }
            if (h.versionMajor != bbipc::kVersionMajor) {
                log(fmt("[ipc] protocol error: client protocol %u.%u, server "
                        "%u.%u -- session rejected",
                        h.versionMajor, h.versionMinor, bbipc::kVersionMajor,
                        bbipc::kVersionMinor));
                respond(h.requestId, h.opcode, bbipc::kStProtocolError);
                return;  // no debug command can run on a mismatched session
            }
            if (!handleRequest(h, payload)) return;  // fatal protocol error: close
        }
    }

    void requestSessionCleanup() {
        DebugCommand cmd;
        cmd.opcode = kOpInternalSessionCleanup;
        cmd.control = true;
        cmd.slot = std::make_shared<CommandSlot>();
        queue.push(cmd);
        std::vector<uint8_t> pl;
        bbipc::WireStatus st = bbipc::kStOk;
        for (int waited = 0; waited < 5000; waited += 25) {
            if (cmd.slot->wait(pl, st, 25)) return;
            if (shutdown.load()) return;
        }
        log("[ipc] session cleanup did not run within 5 s (owner loop busy?)");
    }

    void threadMain() {
        stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        eventAvail = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        readEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ovEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        serverPid = GetCurrentProcessId();
        log(fmt("[ipc] debug IPC server starting on %s (server pid %u)",
                pipeNameFull.c_str(), serverPid));
        for (;;) {
            if (shutdown.load()) break;
            if (!createPipeInstance()) {
                failed.store(true);
                break;
            }
            log(fmt("[ipc] listening on %s", pipeNameFull.c_str()));
            const ConnectResult cr = waitForClient();
            if (cr != ConnectResult::Connected) {
                closePipe();
                if (cr == ConnectResult::Stopped || shutdown.load()) break;
                continue;
            }
            // ---- a client is connected: one session at a time -------------
            sessionId.store(makeSessionId());
            clientPid = 0;
            clientType = 0;
            handshaken.store(false);
            epoch.fetch_add(1);  // every queued/forwarded event belongs to it
            clientConnected.store(true);
            sessionsServed.fetch_add(1);
            log(fmt("[ipc] client connected session=%08X",
                    uint32_t(sessionId.load())));
            serveSession();
            // ---- teardown: nothing from this session may leak (spec 44/45) -
            clientConnected.store(false);
            handshaken.store(false);
            epoch.fetch_add(1);
            {
                std::lock_guard<std::mutex> lk(eventMutex);
                events.clear();
            }
            if (attachObserver) attachObserver(false);
            // Cleanup runs on the OWNER thread (halt / clear breakpoints /
            // abort program) -- the worker never touches the target, and it
            // waits so a new session cannot overtake the cleanup.
            requestSessionCleanup();
            DisconnectNamedPipe(pipe);
            closePipe();
            log(fmt("[ipc] client disconnected session=%08X",
                    uint32_t(sessionId.load())));
            if (shutdown.load()) break;
            // loop: listen again -- a reconnect gets a NEW session id
        }
        log("[ipc] debug IPC server stopped");
        if (stopEvent) CloseHandle(stopEvent);
        if (eventAvail) CloseHandle(eventAvail);
        if (readEvent) CloseHandle(readEvent);
        if (ovEvent) CloseHandle(ovEvent);
        stopEvent = eventAvail = readEvent = ovEvent = nullptr;
    }
};

// ===========================================================================
// worker -> owner bridge: request handling (worker side)
// ===========================================================================
bool DebugIpcServer::Impl::handleRequest(const bbipc::PacketHeader& h,
                                         const std::vector<uint8_t>& payload) {
    // Only HELLO and PING are legal before a successful HELLO (spec 133).
    if (!handshaken.load() && h.opcode != bbipc::kOpHello &&
        h.opcode != bbipc::kOpPing) {
        respond(h.requestId, h.opcode, bbipc::kStProtocolError);
        return true;  // the session may still send a valid HELLO
    }
    if (h.opcode == bbipc::kOpPing) {
        // PING never touches the target (spec 135): answered by the worker so
        // it measures the pipe round trip, not the owner loop.
        if (payload.size() != 8) {
            respond(h.requestId, h.opcode, bbipc::kStProtocolError);
            return true;
        }
        std::vector<uint8_t> out;
        bbipc::PayloadWriter w(out);
        w.u64(bbipc::readLe64(payload.data()));
        respond(h.requestId, h.opcode, bbipc::kStOk, out);
        return true;
    }
    if (!isKnownOpcode(h.opcode)) {
        respond(h.requestId, h.opcode, bbipc::kStInvalidCommand);
        return true;
    }
    DebugCommandArgs args;
    bbipc::WireStatus decodeErr = bbipc::kStOk;
    if (!decodeRequest(h.opcode, payload, args, decodeErr)) {
        respond(h.requestId, h.opcode, decodeErr);
        return true;
    }
    if (h.opcode == bbipc::kOpHello) {
        // The HELLO payload carries the client's declared protocol version --
        // a major mismatch rejects the session before any debug command can
        // run (spec 19/92/133). Minor versions are compatible.
        const uint16_t clientMajor = uint16_t(args.u32a >> 16);
        const uint16_t clientMinor = uint16_t(args.u32a & 0xFFFF);
        clientPid = args.u32b;
        clientType = uint32_t(args.u64a);
        if (clientMajor != bbipc::kVersionMajor) {
            log(fmt("[ipc] protocol error: client protocol %u.%u rejected "
                    "(server %u.%u) -- session closed",
                    clientMajor, clientMinor, bbipc::kVersionMajor,
                    bbipc::kVersionMinor));
            respond(h.requestId, h.opcode, bbipc::kStProtocolError);
            return false;
        }
    }

    // Everything else is executed by the OWNER thread (spec 5/36).
    DebugCommand cmd;
    cmd.requestId = h.requestId;
    cmd.opcode = h.opcode;
    cmd.control = isControlOpcode(h.opcode);
    cmd.args = std::move(args);
    cmd.slot = std::make_shared<CommandSlot>();
    queue.push(cmd);

    std::vector<uint8_t> out;
    bbipc::WireStatus st = bbipc::kStOk;
    const int timeoutMs = (h.opcode == bbipc::kOpProgramEnd)
                              ? opt.programEndTimeoutMs
                              : opt.requestTimeoutMs;
    bool got = false;
    // Deadline-based budget: a slice counter would drift badly, because
    // std::condition_variable::wait_for() can overrun by up to the OS timer
    // granularity (~15.6 ms on Windows) -- 80 x 25 ms slices then measured
    // 2422 ms instead of 2000 ms, so the client got the owner's late answer
    // instead of a Timeout.
    const auto t0 = std::chrono::steady_clock::now();
    const auto deadline =
        t0 + std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 1);
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        const long long leftMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                 now)
                .count();
        const int slice = int(std::min<long long>(25, leftMs > 0 ? leftMs : 1));
        if (cmd.slot->wait(out, st, slice)) {
            got = true;
            break;
        }
        if (shutdown.load()) break;
    }
    const long long elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0)
            .count();
    if (!got) {
        // The client gets Timeout; the owner skips the command if it is still
        // queued, so no stale response can ever appear (spec 42).
        cmd.slot->abandon();
        st = bbipc::kStTimeout;
        out.clear();
        log(fmt("[ipc] request op=0x%04X req=%u timed out after %d ms", h.opcode,
                h.requestId, timeoutMs));
    } else if (elapsedMs > 500) {
        // diagnostic: the owner loop was blocked (GUI stall / heavy command)
        log(fmt("[ipc] slow response op=0x%04X req=%u: %lld ms", h.opcode,
                h.requestId, elapsedMs));
    }
    if (h.opcode == bbipc::kOpHello && got && st == bbipc::kStOk) {
        handshaken.store(true);
        log(fmt("[ipc] HELLO ok: client pid=%u type=%u protocol=%u.%u",
                clientPid, clientType, h.versionMajor, h.versionMinor));
        if (attachObserver) attachObserver(true);
    }
    respond(h.requestId, h.opcode, st, out);
    return true;
}

// ===========================================================================
// owner thread: command execution
// ===========================================================================
namespace {

CommandOutcome executeCommand(const DebugCommand& cmd, IDebugTarget& target,
                              IFlashProgrammer& programmer) {
    CommandOutcome out;
    const DebugCommandArgs& a = cmd.args;
    bbipc::PayloadWriter w(out.payload);
    switch (cmd.opcode) {
    case bbipc::kOpHello: {
        const DebugTargetInfo info = target.info();
        w.u16(bbipc::kVersionMajor);
        w.u16(bbipc::kVersionMinor);
        w.u32(GetCurrentProcessId());
        w.u32(0);  // sessionId placeholder, replaced by the caller below
        uint32_t flags = 0;
        if (info.exactSingleStep) flags |= bbipc::kCapExactSingleStep;
        if (info.executionBreakpoint) flags |= bbipc::kCapExecutionBreakpoint;
        if (info.fpu) flags |= bbipc::kCapFpu;
        if (info.flashProgramming) flags |= bbipc::kCapFlashProgramming;
        if (info.asyncStopEvent) flags |= bbipc::kCapAsyncStopEvent;
        flags |= bbipc::kCapBatchRegisterRead;
        w.u32(flags);
        writeString(w, info.targetName);
        writeString(w, info.boardName);
        writeString(w, info.architecture);
        break;
    }
    case bbipc::kOpGetCapabilities: {
        const DebugTargetInfo info = target.info();
        writeString(w, info.architecture);
        w.u32(bbipc::kWireEndianLittle);  // this model is little-endian
        uint32_t flags = 0;
        if (info.exactSingleStep) flags |= bbipc::kCapExactSingleStep;
        if (info.executionBreakpoint) flags |= bbipc::kCapExecutionBreakpoint;
        if (info.dataWatchpoint) flags |= bbipc::kCapDataWatchpoint;
        if (info.fpu) flags |= bbipc::kCapFpu;
        if (info.flashProgramming) flags |= bbipc::kCapFlashProgramming;
        if (info.asyncStopEvent) flags |= bbipc::kCapAsyncStopEvent;
        flags |= bbipc::kCapBatchRegisterRead;
        w.u32(flags);
        w.u32(info.flashBase);
        w.u32(info.flashSize);
        w.u32(info.flashAliasBase);
        w.u32(info.flashAliasSize);
        w.u32(info.sramBase);
        w.u32(info.sramSize);
        w.u32(info.ccmBase);
        w.u32(info.ccmSize);
        w.u32(bbipc::kMaxMemoryTransfer);
        w.u32(programmer.maxTransfer());
        w.u32(bbipc::kMaxPayload);
        break;
    }
    case bbipc::kOpGetState: {
        w.u32(uint32_t(wireStateOf(target.state())));
        w.u32(uint32_t(wireReasonOf(target.stopInfo().reason)));
        uint64_t pc = 0;
        target.readRegister(DebugRegister::PC, pc);
        w.u32(uint32_t(pc) & ~1u);
        w.u64(target.virtualCycles());
        w.u32(target.firmwareLoaded() ? 1u : 0u);
        break;
    }
    case bbipc::kOpGetStopInfo: {
        const StopInfo si = target.stopInfo();
        w.u32(uint32_t(wireReasonOf(si.reason)));
        w.u32(si.pc);
        break;
    }
    case bbipc::kOpHalt:
        out.status = wireStatusOf(target.halt());
        break;
    case bbipc::kOpResume:
        out.status = wireStatusOf(target.resume());
        break;
    case bbipc::kOpStep:
        out.status = wireStatusOf(target.step());
        break;
    case bbipc::kOpResetHalt:
        out.status = wireStatusOf(target.resetHalt());
        break;
    case bbipc::kOpResetRun:
        out.status = wireStatusOf(target.resetRun());
        break;
    case bbipc::kOpReadRegister: {
        DebugRegister reg{};
        if (!wireRegisterOf(a.u32a, reg)) {
            out.status = bbipc::kStInvalidRegister;
            break;
        }
        uint64_t v = 0;
        out.status = wireStatusOf(target.readRegister(reg, v));
        if (out.status == bbipc::kStOk) w.u64(v);
        break;
    }
    case bbipc::kOpWriteRegister: {
        DebugRegister reg{};
        if (!wireRegisterOf(a.u32a, reg)) {
            out.status = bbipc::kStInvalidRegister;
            break;
        }
        out.status = wireStatusOf(target.writeRegister(reg, a.u64a));
        break;
    }
    case bbipc::kOpReadRegisters: {
        w.u32(uint32_t(a.ids.size()));
        for (uint32_t id : a.ids) {
            DebugRegister reg{};
            uint64_t v = 0;
            bbipc::WireStatus st = bbipc::kStOk;
            if (!wireRegisterOf(id, reg)) {
                st = bbipc::kStInvalidRegister;
            } else {
                st = wireStatusOf(target.readRegister(reg, v));
            }
            w.u32(id);
            w.u64(st == bbipc::kStOk ? v : 0);
            w.u32(uint32_t(st));
        }
        break;
    }
    case bbipc::kOpWriteRegisters: {
        w.u32(uint32_t(a.ids.size()));
        for (size_t i = 0; i < a.ids.size(); i++) {
            DebugRegister reg{};
            bbipc::WireStatus st = bbipc::kStOk;
            if (!wireRegisterOf(a.ids[i], reg)) {
                st = bbipc::kStInvalidRegister;
            } else {
                const uint64_t v =
                    a.bytes.size() >= (i + 1) * 8 ? bbipc::readLe64(
                        a.bytes.data() + i * 8)
                                                  : 0;
                st = wireStatusOf(target.writeRegister(reg, v));
            }
            w.u32(a.ids[i]);
            w.u32(uint32_t(st));
        }
        break;
    }
    case bbipc::kOpReadMemory: {
        const uint32_t len = a.u32b;
        if (len == 0) break;  // empty transfer = Ok no-op (spec 142)
        out.payload.resize(len);
        out.status = wireStatusOf(
            target.readMemory(a.u32a, out.payload.data(), out.payload.size()));
        if (out.status != bbipc::kStOk) out.payload.clear();
        break;
    }
    case bbipc::kOpWriteMemory: {
        if (a.bytes.empty()) break;  // empty transfer = Ok no-op
        // Flash stays Unsupported here -- ordinary debugger memory writes must
        // never program the board (spec 26/47/165).
        out.status = wireStatusOf(
            target.writeMemory(a.u32a, a.bytes.data(), a.bytes.size()));
        break;
    }
    case bbipc::kOpAddBreakpoint:
        out.status = wireStatusOf(target.addBreakpoint(a.u32a));
        break;
    case bbipc::kOpRemoveBreakpoint:
        out.status = wireStatusOf(target.removeBreakpoint(a.u32a));
        break;
    case bbipc::kOpClearBreakpoints:
        target.clearBreakpoints();
        break;
    case bbipc::kOpProgramBegin: {
        uint64_t token = 0;
        const DebugStatus s = programmer.programBegin(token);
        out.status = wireStatusOf(s);
        if (s == DebugStatus::Ok) {
            const DebugTargetInfo info = target.info();
            w.u64(token);
            w.u32(info.flashBase);
            w.u32(info.flashSize);
        }
        break;
    }
    case bbipc::kOpProgramErase:
        out.status = wireStatusOf(
            programmer.programErase(a.u64a, a.u32a, a.u32b));
        break;
    case bbipc::kOpProgramWrite:
        out.status = wireStatusOf(programmer.programWrite(
            a.u64a, a.u32a, a.bytes.empty() ? nullptr : a.bytes.data(),
            a.bytes.size()));
        break;
    case bbipc::kOpProgramEnd: {
        const DebugStatus s = programmer.programEnd(a.u64a);
        out.status = wireStatusOf(s);
        if (s == DebugStatus::Ok) {
            // convenience for test/AGDI: the NEW vector table's SP/reset PC
            uint64_t sp = 0, pc = 0;
            target.readRegister(DebugRegister::SP, sp);
            target.readRegister(DebugRegister::PC, pc);
            w.u32(uint32_t(sp));
            w.u32(uint32_t(pc) & ~1u);
        }
        break;
    }
    case bbipc::kOpProgramAbort:
        out.status = wireStatusOf(programmer.programAbort(a.u64a));
        break;
    default:
        out.status = bbipc::kStInvalidCommand;
        break;
    }
    return out;
}

}  // namespace

// ===========================================================================
// public API
// ===========================================================================
DebugIpcServer::DebugIpcServer(Options options)
    : impl_(std::make_unique<Impl>()) {
    impl_->opt = std::move(options);
    impl_->pipeNameFull = normalizePipeName(impl_->opt.pipeName);
}

DebugIpcServer::~DebugIpcServer() { stop(); }

bool DebugIpcServer::start() {
    Impl& im = *impl_;
    if (im.started.load()) return true;
    im.shutdown.store(false);
    im.started.store(true);
    im.thread = std::thread([&im] { im.threadMain(); });
    return true;
}

void DebugIpcServer::stop() {
    Impl& im = *impl_;
    if (!im.started.load()) return;
    im.shutdown.store(true);
    // wake the worker: stop event + cancel any pending overlapped I/O
    if (im.stopEvent) SetEvent(im.stopEvent);
    if (im.eventAvail) SetEvent(im.eventAvail);
    if (im.pipe != INVALID_HANDLE_VALUE) CancelIoEx(im.pipe, nullptr);
    if (im.thread.joinable()) im.thread.join();
    im.started.store(false);
}

bool DebugIpcServer::running() const { return impl_->started.load(); }

void DebugIpcServer::processPendingCommands(IDebugTarget& target,
                                            IFlashProgrammer& programmer) {
    Impl& im = *impl_;
    // 1) drain worker log lines on the owner thread (the file logger is
    //    single-threaded: the worker may only queue text)
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lk(im.logMutex);
        lines.swap(im.logLines);
    }
    for (const std::string& l : lines) {
        if (im.logSink) im.logSink(l);
    }
    // 2) execute queued commands with a strict work budget: control commands
    //    (HALT/RESET/STEP/RESUME/HELLO) first, then normal ones; always at
    //    least one command so a waiting client cannot deadlock, never more
    //    than kMaxCommandsPerPump / ~2 ms so the GUI and the simulation can
    //    never be starved (spec 111/112).
    const auto t0 = std::chrono::steady_clock::now();
    int processed = 0;
    for (;;) {
        if (processed >= kMaxCommandsPerPump) break;
        if (processed > 0) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
            if (ms >= kPumpBudgetMs) break;
        }
        DebugCommand cmd;
        if (!im.queue.tryPop(cmd)) break;
        if (cmd.slot && cmd.slot->abandoned()) continue;  // timed out client
        ++processed;

        if (cmd.opcode == kOpInternalSessionCleanup) {
            // Disconnect policy (spec 137/158): halt, clear debugger
            // breakpoints, abort a program transaction -- never reset, the
            // current firmware stays in the virtual flash.
            target.halt();
            target.clearBreakpoints();
            programmer.abortActive();
            if (cmd.slot) cmd.slot->complete(bbipc::kStOk, {});
            continue;
        }
        CommandOutcome outcome = executeCommand(cmd, target, programmer);
        if (cmd.opcode == bbipc::kOpHello &&
            outcome.status == bbipc::kStOk) {
            // patch the real session id into the HELLO response (offset 8)
            if (outcome.payload.size() >= 12) {
                bbipc::writeLe32(outcome.payload.data() + 8,
                                 uint32_t(im.sessionId.load()));
            }
        }
        if (cmd.slot) cmd.slot->complete(outcome.status, std::move(outcome.payload));
    }
}

void DebugIpcServer::waitForCommands(int timeoutMs) const {
    impl_->queue.waitForWork(timeoutMs);
}

void DebugIpcServer::notifyTargetStopped(const StopInfo& info,
                                         uint64_t virtualCycles) {
    Impl& im = *impl_;
    if (!im.clientConnected.load()) return;  // no session: never queue events
    Impl::DebugEvent ev;
    ev.opcode = (info.reason == StopReason::Fault) ? bbipc::kEventTargetFaulted
                                                   : bbipc::kEventTargetStopped;
    ev.reason = uint32_t(wireReasonOf(info.reason));
    ev.pc = info.pc;
    ev.cycles = virtualCycles;
    {
        std::lock_guard<std::mutex> lk(im.eventMutex);
        im.events.emplace_back(im.epoch.load(), ev);
    }
    if (im.eventAvail) SetEvent(im.eventAvail);  // wake the pipe worker
}

bool DebugIpcServer::clientConnected() const {
    return impl_->clientConnected.load();
}

uint64_t DebugIpcServer::sessionId() const { return impl_->sessionId.load(); }

uint64_t DebugIpcServer::sessionsServed() const {
    return impl_->sessionsServed.load();
}

void DebugIpcServer::setLogSink(std::function<void(const std::string&)> sink) {
    impl_->logSink = std::move(sink);
}

void DebugIpcServer::setAttachObserver(std::function<void(bool)> observer) {
    impl_->attachObserver = std::move(observer);
}