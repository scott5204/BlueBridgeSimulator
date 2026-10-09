// ============================================================================
// Stage 7-2A acceptance test: Debug IPC (Windows Named Pipe) + virtual flash
// programming over IPC.
//
// The architecture is really exercised, never bypassed:
//
//   client THREAD  --real pipe bytes-->  DebugIpcServer pipe worker
//        ^                                    |  (decode only; never touches the
//        |                                    v   Simulator / Unicorn)
//        |                             DebugCommandQueue
//        |                                    |
//        +-- responses / TARGET_STOPPED -- Simulator OWNER thread (this file's
//                                        main loop) -> SimulatorDebugTarget /
//                                        SimulatorFlashProgrammer -> Simulator
//
// The client uses the SAME wire codec as the server (bbipc::encodeHeader /
// decodeHeader / PayloadWriter / PayloadReader) but exchanges real bytes over a
// real Named Pipe, so framing bugs cannot hide behind shared structs.
//
// Exit code 0 = PASS. Every phase prints PASS/FAIL.
// ============================================================================
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "debug/SimulatorDebugTarget.h"
#include "debug/SimulatorFlashProgrammer.h"
#include "debug/ipc/DebugIpcCodec.h"
#include "debug/ipc/DebugIpcServer.h"
#include "loader/HexLoader.h"
#include "sim/Simulator.h"

namespace {

std::atomic<int> g_failures{0};
std::mutex g_print;
std::mutex g_perf;

void say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_print);
    std::fputs(buf, stdout);
    std::fflush(stdout);
}

void check(bool ok, const char* what) {
    if (!ok) {
        say("  FAIL: %s\n", what);
        g_failures.fetch_add(1);
    }
}

// Diagnostic: the Windows heap validator, used once at the very end of the run
// (a corrupt heap must fail the test even if every protocol check passed).
void heapCheck(const char* where) {
    if (!HeapValidate(GetProcessHeap(), 0, nullptr)) {
        say("  HEAP CORRUPTION detected after %s\n", where);
        g_failures.fetch_add(1);
    }
}

struct PhaseGuard {
    const char* name;
    int before;
    explicit PhaseGuard(const char* n) : name(n), before(g_failures.load()) {
        say("\n=== %s\n", name);
    }
    ~PhaseGuard() {
        say("=== %s: %s\n", name,
            g_failures.load() == before ? "PASS" : "FAIL");
    }
};

std::wstring toWide(const std::string& s) {
    std::wstring w;
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n > 0) {
        w.resize(size_t(n));
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        w.resize(size_t(n) - 1);
    }
    return w;
}

// ---------------------------------------------------------------------------
// firmware layout (read from the .debugmap of each build, never hard-coded)
// ---------------------------------------------------------------------------
struct FwLayout {
    bool valid = false;
    uint32_t reset = 0;
    uint32_t code = 0;        // the differing instruction
    uint32_t magicAddr = 0;
    uint32_t resultAddr = 0;
    uint32_t magicValue = 0;  // A: 0xA1A1A1A1 / B: 0xB2B2B2B2
    uint32_t resultValue = 0; // A: 0x11 / B: 0x22
    uint32_t vecSp = 0, vecPc = 0;
    uint32_t loopAddr = 0;
    uint16_t opcode = 0;      // the halfword at `code`
};

FwLayout captureLayoutOnOwner(SimulatorDebugTarget& t) {
    FwLayout l;
    uint32_t map[16] = {};
    if (t.readMemory(0x08000200, map, sizeof(map)) != DebugStatus::Ok) return l;
    if (map[0] != 0x50524F47 || map[15] != 0x50524F47) return l;
    l.reset = map[1] & ~1u;
    l.code = map[2] & ~1u;
    l.magicAddr = map[3];
    l.resultAddr = map[4];
    l.magicValue = map[5];
    l.resultValue = map[6];
    l.loopAddr = map[8] & ~1u;
    uint32_t sp = 0, pc = 0;
    t.readMemory(0x08000000, &sp, 4);
    t.readMemory(0x08000004, &pc, 4);
    l.vecSp = sp;
    l.vecPc = pc & ~1u;
    t.readMemory(l.code, &l.opcode, 2);
    l.valid = true;
    return l;
}

// owner-thread helper: reset, run the tiny program with exact single steps and
// read what it published in SRAM
bool runProgramOnOwner(SimulatorDebugTarget& t, const FwLayout& l,
                       uint32_t& magic, uint32_t& result) {
    if (t.resetHalt() != DebugStatus::Ok) return false;
    for (int i = 0; i < 6; i++) {
        if (t.step() != DebugStatus::Ok) return false;
    }
    return t.readMemory(l.magicAddr, &magic, 4) == DebugStatus::Ok &&
           t.readMemory(l.resultAddr, &result, 4) == DebugStatus::Ok;
}

// ---------------------------------------------------------------------------
// IPC test client (overlapped reads with deadlines). One instance per session.
// ---------------------------------------------------------------------------
class TestClient {
public:
    struct Packet {
        uint16_t kind = 0;
        uint16_t opcode = 0;
        uint32_t requestId = 0;
        uint32_t status = 0;
        std::vector<uint8_t> payload;
        bool ok = false;
    };

    explicit TestClient(std::string pipeName) : pipeName_(std::move(pipeName)) {
        ovEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    ~TestClient() {
        disconnect();
        if (ovEvent_) CloseHandle(ovEvent_);
    }

    TestClient(const TestClient&) = delete;
    TestClient& operator=(const TestClient&) = delete;

    bool connect(int timeoutMs = 5000) {
        const std::wstring wname = toWide(pipeName_);
        QElapsedTimer t;
        t.start();
        for (;;) {
            HANDLE h = CreateFileW(wname.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                   nullptr, OPEN_EXISTING,
                                   FILE_FLAG_OVERLAPPED, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                pipe_ = h;
                DWORD mode = PIPE_READMODE_BYTE;
                SetNamedPipeHandleState(pipe_, &mode, nullptr, nullptr);
                return true;
            }
            const DWORD err = GetLastError();
            if (err == ERROR_PIPE_BUSY) {
                WaitNamedPipeW(wname.c_str(), 100);
            } else if (err == ERROR_FILE_NOT_FOUND) {
                QThread::msleep(5);
            } else {
                say("  client: CreateFile failed (win32 err=%lu)\n", err);
                return false;
            }
            if (t.elapsed() > timeoutMs) {
                say("  client: connect timeout after %d ms\n", timeoutMs);
                return false;
            }
        }
    }

    void disconnect() {
        if (pipe_ != INVALID_HANDLE_VALUE) {
            CancelIoEx(pipe_, nullptr);
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        }
        clearQueued();
        readBuf_.clear();
        readPos_ = 0;
        lastError_ = 0;
    }

    bool connected() const { return pipe_ != INVALID_HANDLE_VALUE; }

    // ---- raw wire I/O -----------------------------------------------------
    bool sendRequest(uint16_t opcode, const std::vector<uint8_t>& payload,
                     uint32_t requestId) {
        bbipc::PacketHeader h;
        h.packetKind = bbipc::kPacketRequest;
        h.opcode = opcode;
        h.requestId = requestId;
        h.payloadSize = uint32_t(payload.size());
        h.status = 0;
        uint8_t raw[bbipc::kHeaderSize];
        bbipc::encodeHeader(raw, h);
        return writeExact(raw, sizeof(raw)) &&
               (payload.empty() || writeExact(payload.data(), payload.size()));
    }

    bool sendRaw(const void* data, size_t size) {
        return writeExact(data, size);
    }

    bool readPacket(Packet& p, int timeoutMs) {
        QElapsedTimer t;
        t.start();
        if (!fillAtLeast(bbipc::kHeaderSize, timeoutMs, t)) return false;
        const uint8_t* h = readBuf_.data() + readPos_;
        bbipc::PacketHeader ph;
        if (!bbipc::decodeHeader(h, ph)) {
            lastError_ = 0xDEAD;  // protocol violation
            return false;
        }
        if (ph.payloadSize > bbipc::kMaxPayload) {
            lastError_ = 0xDEAD;
            return false;
        }
        const size_t total = bbipc::kHeaderSize + ph.payloadSize;
        if (!fillAtLeast(total, timeoutMs, t)) return false;
        const uint8_t* base = readBuf_.data() + readPos_;
        p.kind = ph.packetKind;
        p.opcode = ph.opcode;
        p.requestId = ph.requestId;
        p.status = ph.status;
        p.payload.clear();
        if (ph.payloadSize) {
            p.payload.assign(base + bbipc::kHeaderSize,
                             base + bbipc::kHeaderSize + ph.payloadSize);
        }
        consume(total);
        p.ok = true;
        return true;
    }

    uint32_t lastIoError() const { return lastError_; }

    bool waitResponse(uint32_t requestId, Packet& p, int timeoutMs = 6000) {
        auto it = responses_.find(requestId);
        if (it != responses_.end()) {
            p = it->second;
            responses_.erase(it);
            return true;
        }
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < timeoutMs) {
            const int left = int(timeoutMs - t.elapsed());
            Packet q;
            if (!readPacket(q, left > 0 ? left : 1)) return false;
            if (q.kind == bbipc::kPacketResponse) {
                if (q.requestId == requestId) {
                    p = q;
                    return true;
                }
                responses_[q.requestId] = q;
            } else if (q.kind == bbipc::kPacketEvent) {
                events_.push_back(q);
            }
        }
        return false;
    }

    bool waitEvent(uint16_t opcode, Packet& p, int timeoutMs = 6000) {
        for (size_t i = 0; i < events_.size(); i++) {
            if (events_[i].opcode == opcode) {
                p = events_[i];
                events_.erase(events_.begin() + long(i));
                return true;
            }
        }
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < timeoutMs) {
            const int left = int(timeoutMs - t.elapsed());
            Packet q;
            if (!readPacket(q, left > 0 ? left : 1)) return false;
            if (q.kind == bbipc::kPacketEvent) {
                if (q.opcode == opcode) {
                    p = q;
                    return true;
                }
                events_.push_back(q);
            } else if (q.kind == bbipc::kPacketResponse) {
                responses_[q.requestId] = q;
            }
        }
        return false;
    }

    void clearQueued() {
        events_.clear();
        responses_.clear();
    }

    // RESET_HALT / RESET_RUN / STEP / HALT each publish a TARGET_STOPPED event
    // of their own (by design: every stop reason produces one). Drain them so
    // the next waitEvent() only sees the event under test.
    int drainStopEvents(int firstWaitMs = 300) {
        int n = 0;
        TestClient::Packet p;
        int wait = firstWaitMs;
        while (waitEvent(bbipc::kEventTargetStopped, p, wait)) {
            ++n;
            wait = 50;
        }
        return n;
    }

    uint32_t nextRequestId() { return ++lastRequestId_; }

    // ---- request helpers --------------------------------------------------
    bool req(uint16_t opcode, const std::vector<uint8_t>& payload,
             bbipc::WireStatus expect, Packet& out, const char* what) {
        const uint32_t id = nextRequestId();
        if (!sendRequest(opcode, payload, id)) {
            say("  send failed: %s (win32 err=%u)\n", what, lastError_);
            check(false, what);
            return false;
        }
        Packet p;
        if (!waitResponse(id, p)) {
            say("  no response: %s (win32 err=%u)\n", what, lastError_);
            check(false, what);
            return false;
        }
        if (p.status != uint32_t(expect)) {
            say("  %s: status=%u expected %u\n", what, p.status,
                unsigned(expect));
            check(false, what);
            return false;
        }
        out = p;
        return true;
    }

    bool reqOk(uint16_t opcode, const std::vector<uint8_t>& payload,
               Packet& out, const char* what) {
        return req(opcode, payload, bbipc::kStOk, out, what);
    }

    bool hello(uint16_t major, uint16_t minor, uint32_t clientType, Packet& out,
               const char* what) {
        std::vector<uint8_t> pl;
        bbipc::PayloadWriter w(pl);
        w.u16(major);
        w.u16(minor);
        w.u32(GetCurrentProcessId());
        w.u32(clientType);
        return req(bbipc::kOpHello, pl, bbipc::kStOk, out, what);
    }

    // ---- typed conveniences ----------------------------------------------
    bool readRegister(uint32_t regId, uint64_t& value, const char* what) {
        std::vector<uint8_t> pl;
        bbipc::PayloadWriter w(pl);
        w.u32(regId);
        Packet p;
        if (!reqOk(bbipc::kOpReadRegister, pl, p, what)) return false;
        bbipc::PayloadReader r(p.payload.data(), p.payload.size());
        if (!r.u64(value) || !r.ok()) {
            check(false, what);
            return false;
        }
        return true;
    }

    bool writeRegister(uint32_t regId, uint64_t value, const char* what) {
        std::vector<uint8_t> pl;
        bbipc::PayloadWriter w(pl);
        w.u32(regId);
        w.u64(value);
        Packet p;
        return reqOk(bbipc::kOpWriteRegister, pl, p, what);
    }

    bool readMemory(uint32_t addr, void* dst, uint32_t len, const char* what) {
        std::vector<uint8_t> pl;
        bbipc::PayloadWriter w(pl);
        w.u32(addr);
        w.u32(len);
        Packet p;
        if (!reqOk(bbipc::kOpReadMemory, pl, p, what)) return false;
        if (p.payload.size() != len) {
            check(false, what);
            return false;
        }
        std::memcpy(dst, p.payload.data(), len);
        return true;
    }

    bool readMemoryExpect(uint32_t addr, uint32_t len, bbipc::WireStatus expect,
                          const char* what) {
        std::vector<uint8_t> pl;
        bbipc::PayloadWriter w(pl);
        w.u32(addr);
        w.u32(len);
        Packet p;
        return req(bbipc::kOpReadMemory, pl, expect, p, what);
    }

    bool writeMemory(uint32_t addr, const void* data, uint32_t len,
                     bbipc::WireStatus expect, const char* what) {
        std::vector<uint8_t> pl;
        bbipc::PayloadWriter w(pl);
        w.u32(addr);
        w.u32(len);
        w.bytes(data, len);
        Packet p;
        return req(bbipc::kOpWriteMemory, pl, expect, p, what);
    }

    bool control(uint16_t opcode, const char* what) {
        Packet p;
        return reqOk(opcode, {}, p, what);
    }

    // one-u32-argument / empty-payload convenience requests
    bool oneU32(uint16_t opcode, uint32_t value, const char* what) {
        std::vector<uint8_t> pl;
        bbipc::PayloadWriter w(pl);
        w.u32(value);
        Packet p;
        return reqOk(opcode, pl, p, what);
    }

    bool noPayload(uint16_t opcode, const char* what) {
        Packet p;
        return reqOk(opcode, {}, p, what);
    }

    bool state(uint32_t& wireState, uint32_t& reason, uint32_t& pc,
               uint64_t& cycles, const char* what) {
        Packet p;
        if (!reqOk(bbipc::kOpGetState, {}, p, what)) return false;
        bbipc::PayloadReader r(p.payload.data(), p.payload.size());
        uint32_t fw = 0;
        if (!r.u32(wireState) || !r.u32(reason) || !r.u32(pc) ||
            !r.u64(cycles) || !r.u32(fw) || !r.ok()) {
            check(false, what);
            return false;
        }
        return true;
    }

private:
    // ---- framed byte reader ----------------------------------------------
    // Bytes land in readBuf_; packets are cut out of it, so a deadline can
    // never desync the framing. A pending read is cancelled ONLY when it has
    // transferred nothing, and the cancellation is always reaped with
    // GetOverlappedResult before the OVERLAPPED (a stack local) goes away --
    // an unreaped cancelled I/O writing into a dead stack frame is exactly the
    // heap/stack corruption this test used to trigger.
    bool fillAtLeast(size_t n, int timeoutMs, QElapsedTimer& t) {
        while (readBuf_.size() - readPos_ < n) {
            if (!pumpOnce(timeoutMs, t)) return false;
        }
        return true;
    }

    bool pumpOnce(int timeoutMs, QElapsedTimer& t) {
        uint8_t tmp[4096];
        OVERLAPPED ov;
        std::memset(&ov, 0, sizeof(ov));
        ov.hEvent = ovEvent_;
        ResetEvent(ovEvent_);
        const BOOL ok = ReadFile(pipe_, tmp, sizeof(tmp), nullptr, &ov);
        DWORD done = 0;
        if (!ok) {
            const DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                lastError_ = err;
                return false;
            }
            for (;;) {
                const int left = int(timeoutMs - t.elapsed());
                if (left <= 0) {
                    CancelIoEx(pipe_, &ov);
                    GetOverlappedResult(pipe_, &ov, &done, TRUE);  // reap!
                    return false;  // timeout; nothing was consumed
                }
                if (WaitForSingleObject(ovEvent_, DWORD(left)) ==
                    WAIT_OBJECT_0) {
                    break;
                }
            }
            if (!GetOverlappedResult(pipe_, &ov, &done, FALSE)) {
                lastError_ = GetLastError();
                return false;
            }
        } else {
            done = DWORD(ov.InternalHigh);
        }
        if (done == 0) {
            lastError_ = ERROR_BROKEN_PIPE;
            return false;
        }
        readBuf_.insert(readBuf_.end(), tmp, tmp + done);
        return true;
    }

    void consume(size_t n) {
        readPos_ += n;
        if (readPos_ >= readBuf_.size()) {
            readBuf_.clear();
            readPos_ = 0;
        } else if (readPos_ > 65536) {
            readBuf_.erase(readBuf_.begin(),
                           readBuf_.begin() + std::ptrdiff_t(readPos_));
            readPos_ = 0;
        }
    }

    bool writeExact(const void* src, size_t size) {
        const uint8_t* in = static_cast<const uint8_t*>(src);
        size_t sent = 0;
        while (sent < size) {
            OVERLAPPED ov;
            std::memset(&ov, 0, sizeof(ov));
            ov.hEvent = ovEvent_;
            ResetEvent(ovEvent_);
            const BOOL ok =
                WriteFile(pipe_, in + sent, DWORD(size - sent), nullptr, &ov);
            DWORD done = 0;
            if (!ok) {
                const DWORD err = GetLastError();
                if (err != ERROR_IO_PENDING) {
                    lastError_ = err;
                    return false;
                }
                if (WaitForSingleObject(ovEvent_, 4000) != WAIT_OBJECT_0) {
                    CancelIoEx(pipe_, &ov);
                    GetOverlappedResult(pipe_, &ov, &done, TRUE);  // reap
                    lastError_ = ERROR_TIMEOUT;
                    return false;
                }
                if (!GetOverlappedResult(pipe_, &ov, &done, FALSE)) {
                    lastError_ = GetLastError();
                    return false;
                }
            } else {
                done = DWORD(ov.InternalHigh);
            }
            if (done == 0) {
                lastError_ = ERROR_BROKEN_PIPE;
                return false;
            }
            sent += done;
        }
        return true;
    }

    std::string pipeName_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE ovEvent_ = nullptr;
    uint32_t lastRequestId_ = 0;
    uint32_t lastError_ = 0;
    std::deque<Packet> events_;
    std::map<uint32_t, Packet> responses_;
    std::vector<uint8_t> readBuf_;  // carry-over bytes (see pumpOnce)
    size_t readPos_ = 0;
};

// ---------------------------------------------------------------------------
// payload helpers
// ---------------------------------------------------------------------------
std::vector<uint8_t> u32Payload(uint32_t v) {
    std::vector<uint8_t> pl;
    bbipc::PayloadWriter w(pl);
    w.u32(v);
    return pl;
}

std::vector<uint8_t> u64Payload(uint64_t v) {
    std::vector<uint8_t> pl;
    bbipc::PayloadWriter w(pl);
    w.u64(v);
    return pl;
}

std::vector<uint8_t> tokenU32U32(uint64_t token, uint32_t a, uint32_t b) {
    std::vector<uint8_t> pl;
    bbipc::PayloadWriter w(pl);
    w.u64(token);
    w.u32(a);
    w.u32(b);
    return pl;
}

std::vector<uint8_t> programWritePayload(uint64_t token, uint32_t addr,
                                         const uint8_t* data, uint32_t len) {
    std::vector<uint8_t> pl;
    bbipc::PayloadWriter w(pl);
    w.u64(token);
    w.u32(addr);
    w.u32(len);
    w.bytes(data, len);
    return pl;
}

// full 128 KiB image built from a hex file (0xFF outside its segments)
std::vector<uint8_t> buildImageFromHex(const QString& hexPath, bool& ok) {
    std::vector<uint8_t> image(128 * 1024, 0xFF);
    FirmwareImage img;
    std::string err;
    ok = false;
    if (!HexLoader::loadFile(hexPath.toStdString(), img, err)) {
        say("  HexLoader failed: %s\n", err.c_str());
        return image;
    }
    for (const auto& seg : img.segments) {
        if (seg.address < 0x08000000u ||
            uint64_t(seg.address) + seg.data.size() > 0x08020000u) {
            say("  hex segment outside flash\n");
            return image;
        }
        std::memcpy(image.data() + (seg.address - 0x08000000u), seg.data.data(),
                    seg.data.size());
    }
    ok = true;
    return image;
}

struct Ctx {
    FwLayout a, b;
    std::vector<uint8_t> imageA, imageB;
    uint32_t directPc = 0, directSp = 0, directR0 = 0, directXpsr = 0;
    uint32_t lastSessionId = 0;
    std::string pipeName;
};

// client-side equivalent of runProgramOnOwner, entirely over IPC
bool runAndCheckOverIpc(TestClient& c, const Ctx& ctx, const FwLayout& want,
                        const char* what) {
    bool ok = true;
    ok = ok && c.control(bbipc::kOpResetHalt, "RESET_HALT");
    for (int i = 0; i < 6; i++) ok = ok && c.control(bbipc::kOpStep, "STEP");
    uint32_t magic = 0, result = 0;
    ok = ok && c.readMemory(want.magicAddr, &magic, 4, "read magic");
    ok = ok && c.readMemory(want.resultAddr, &result, 4, "read result");
    if (!ok) return false;
    check(magic == want.magicValue, what);
    check(result == want.resultValue, what);
    if (magic != want.magicValue || result != want.resultValue) {
        say("  %s: magic=0x%08X (want 0x%08X) result=0x%X (want 0x%X)\n", what,
            magic, want.magicValue, result, want.resultValue);
        return false;
    }
    return true;
}

// Opens a session and does HELLO; returns false if anything failed.
bool openSession(TestClient& c, Ctx& ctx, const char* what) {
    if (!c.connect()) {
        check(false, what);
        return false;
    }
    TestClient::Packet p;
    if (!c.hello(1, 0, bbipc::kClientTypeTest, p, what)) return false;
    bbipc::PayloadReader r(p.payload.data(), p.payload.size());
    uint16_t maj = 0, min = 0, tlen = 0;
    uint32_t pid = 0, sid = 0, flags = 0;
    r.u16(maj);
    r.u16(min);
    r.u32(pid);
    r.u32(sid);
    r.u32(flags);
    r.u16(tlen);
    ctx.lastSessionId = sid;
    return true;
}

// ===========================================================================
// phases
// ===========================================================================
bool programOverIpc(TestClient& c, const std::vector<uint8_t>& image,
                    const char* what);

void phaseHelloAndCaps(TestClient& c, Ctx& ctx) {
    PhaseGuard ph("A connect + HELLO + capabilities");
    check(c.connect(), "client connect");
    TestClient::Packet p;
    check(c.hello(1, 0, bbipc::kClientTypeTest, p, "HELLO"), "HELLO ok");
    if (p.ok) {
        bbipc::PayloadReader r(p.payload.data(), p.payload.size());
        uint16_t maj = 0, min = 0, tlen = 0, blen = 0, alen = 0;
        uint32_t pid = 0, sid = 0, flags = 0;
        r.u16(maj);
        r.u16(min);
        r.u32(pid);
        r.u32(sid);
        r.u32(flags);
        r.u16(tlen);
        std::vector<char> tbuf(size_t(tlen) + 1, 0);
        r.bytes(tbuf.data(), tlen);
        r.u16(blen);
        std::vector<char> bbuf(size_t(blen) + 1, 0);
        r.bytes(bbuf.data(), blen);
        r.u16(alen);
        std::vector<char> abuf(size_t(alen) + 1, 0);
        r.bytes(abuf.data(), alen);
        check(r.ok(), "HELLO payload well formed");
        check(maj == 1 && min == 0, "HELLO protocol 1.0");
        check(pid != 0 && sid != 0, "HELLO server pid + session id");
        check(std::string(tbuf.data()) == "STM32G431RBT6", "HELLO target name");
        check(std::string(bbuf.data()) == "CT117E-M4", "HELLO board name");
        check(std::string(abuf.data()) == "ARM Cortex-M4F", "HELLO architecture");
        check((flags & bbipc::kCapExactSingleStep) != 0,
              "HELLO capability: exact single step");
        check((flags & bbipc::kCapFlashProgramming) != 0,
              "HELLO capability: flash programming");
        check((flags & bbipc::kCapAsyncStopEvent) != 0,
              "HELLO capability: async stop events");
        ctx.lastSessionId = sid;
    }

    check(c.reqOk(bbipc::kOpGetCapabilities, {}, p, "GET_CAPABILITIES"),
          "GET_CAPABILITIES ok");
    if (p.ok) {
        bbipc::PayloadReader r(p.payload.data(), p.payload.size());
        uint16_t alen = 0;
        r.u16(alen);
        std::vector<char> abuf(size_t(alen) + 1, 0);
        r.bytes(abuf.data(), alen);
        uint32_t endian = 0, flags = 0;
        uint32_t fb = 0, fs = 0, ab = 0, asz = 0, sb = 0, ss = 0, cb = 0,
                 cs = 0, mm = 0, pm = 0, mp = 0;
        r.u32(endian);
        r.u32(flags);
        r.u32(fb);
        r.u32(fs);
        r.u32(ab);
        r.u32(asz);
        r.u32(sb);
        r.u32(ss);
        r.u32(cb);
        r.u32(cs);
        r.u32(mm);
        r.u32(pm);
        r.u32(mp);
        check(r.ok(), "capabilities payload well formed");
        check(std::string(abuf.data()) == "ARM Cortex-M4F",
              "capabilities architecture");
        check(endian == bbipc::kWireEndianLittle, "capabilities little endian");
        check(fb == 0x08000000u && fs == 128u * 1024u,
              "capabilities flash range matches the SoC model");
        check(ab == 0x00000000u && asz == 128u * 1024u,
              "capabilities flash alias range");
        check(sb == 0x20000000u && ss == 32u * 1024u,
              "capabilities SRAM range matches the SoC model");
        check(cb == 0x10000000u && cs == 16u * 1024u,
              "capabilities CCM range matches the SoC model");
        check(mm == 65536u && pm == 65536u && mp == 1048576u,
              "capabilities transfer limits");
        check((flags & bbipc::kCapDataWatchpoint) == 0,
              "capabilities: no data watchpoint in this stage");
        check((flags & bbipc::kCapExecutionBreakpoint) != 0,
              "capabilities: execution breakpoints");
    }
    c.disconnect();
}

void phaseState(TestClient& c, Ctx& ctx) {
    PhaseGuard ph("B GET_STATE + GET_STOP_INFO");
    if (!openSession(c, ctx, "open session")) return;
    uint32_t st = 0, reason = 0, pc = 0;
    uint64_t cycles = 0;
    check(c.state(st, reason, pc, cycles, "GET_STATE"),
          "GET_STATE ok");
    check(st == bbipc::kWireStateHalted, "state == Halted");
    check(pc == ctx.directPc, "GET_STATE PC == direct PC");
    TestClient::Packet p;
    check(c.reqOk(bbipc::kOpGetStopInfo, {}, p, "GET_STOP_INFO"),
          "GET_STOP_INFO ok");
    if (p.ok) {
        bbipc::PayloadReader r(p.payload.data(), p.payload.size());
        uint32_t rsn = 0, stopPc = 0;
        r.u32(rsn);
        r.u32(stopPc);
        check(r.ok(), "GET_STOP_INFO payload well formed");
    }
    c.disconnect();
}

void phaseRegisters(TestClient& c, Ctx& ctx) {
    PhaseGuard ph("C/D/E register read/write + batch read");
    if (!openSession(c, ctx, "open session")) return;
    uint64_t pc = 0, sp = 0, r0 = 0;
    check(c.readRegister(bbipc::kWireRegPC, pc, "read PC"), "read PC ok");
    check((uint32_t(pc) & ~1u) == ctx.directPc, "PC matches direct read");
    check(c.readRegister(bbipc::kWireRegSP, sp, "read SP"), "read SP ok");
    check(uint32_t(sp) == ctx.directSp, "SP matches direct read");
    check(c.readRegister(bbipc::kWireRegR0, r0, "read R0"), "read R0 ok");
    check(uint32_t(r0) == ctx.directR0, "R0 matches direct read");

    check(c.writeRegister(bbipc::kWireRegR0, 0x12345678ull, "write R0"),
          "write R0 ok");
    uint64_t back = 0;
    check(c.readRegister(bbipc::kWireRegR0, back, "read R0 back") &&
              back == 0x12345678ull,
          "R0 read-back == 0x12345678");

    // batch read: R0,R1,SP,LR,PC,XPSR
    const uint32_t ids[6] = {bbipc::kWireRegR0, 1, bbipc::kWireRegSP,
                             bbipc::kWireRegLR, bbipc::kWireRegPC,
                             bbipc::kWireRegXPSR};
    std::vector<uint8_t> pl;
    bbipc::PayloadWriter w(pl);
    w.u32(6);
    for (uint32_t id : ids) w.u32(id);
    TestClient::Packet p;
    check(c.reqOk(bbipc::kOpReadRegisters, pl, p, "READ_REGISTERS"),
          "READ_REGISTERS ok");
    if (p.ok) {
        bbipc::PayloadReader r(p.payload.data(), p.payload.size());
        uint32_t count = 0;
        r.u32(count);
        check(count == 6, "batch read returns 6 entries");
        bool allOk = true;
        for (int i = 0; i < 6 && r.ok(); i++) {
            uint32_t id = 0, st = 0;
            uint64_t v = 0;
            r.u32(id);
            r.u64(v);
            r.u32(st);
            allOk = allOk && id == ids[i] && st == bbipc::kStOk;
            if (ids[i] == bbipc::kWireRegR0) {
                check(v == 0x12345678ull, "batch R0 value");
            }
            if (ids[i] == bbipc::kWireRegPC) {
                check((uint32_t(v) & ~1u) == ctx.directPc, "batch PC value");
            }
            if (ids[i] == bbipc::kWireRegSP) {
                check(uint32_t(v) == ctx.directSp, "batch SP value");
            }
        }
        check(allOk, "batch register statuses all Ok");
    }
    c.disconnect();
}

void phaseMemory(TestClient& c, Ctx& ctx) {
    PhaseGuard ph("F/G/H/I memory read/write, unaligned, invalid, flash");
    if (!openSession(c, ctx, "open session")) return;
    const uint32_t base = 0x20000400u;
    const uint32_t sizes[6] = {1, 2, 4, 16, 256, 4096};
    for (uint32_t size : sizes) {
        std::vector<uint8_t> data(size);
        for (uint32_t i = 0; i < size; i++) {
            data[i] = uint8_t(0xA5 ^ (i * 7));
        }
        char what[96];
        std::snprintf(what, sizeof(what), "WRITE_MEMORY %u bytes", size);
        check(c.writeMemory(base, data.data(), size, bbipc::kStOk, what), what);
        std::vector<uint8_t> back(size, 0);
        std::snprintf(what, sizeof(what), "READ_MEMORY %u bytes", size);
        check(c.readMemory(base, back.data(), size, what), what);
        check(back == data, "memory read-back matches (sizes 1..4096)");
    }

    // unaligned access
    const uint8_t unaligned[4] = {0x11, 0x22, 0x33, 0x44};
    check(c.writeMemory(0x20000103u, unaligned, 4, bbipc::kStOk,
                        "unaligned WRITE_MEMORY 0x20000103"),
          "unaligned write ok");
    uint8_t backU[4] = {};
    check(c.readMemory(0x20000103u, backU, 4, "unaligned READ_MEMORY"),
          "unaligned read ok");
    check(std::memcmp(backU, unaligned, 4) == 0, "unaligned data matches");

    // CCM (0x10000000, 16 KiB) is writable through the debugger memory path
    uint8_t ccm[32];
    for (size_t i = 0; i < sizeof(ccm); i++) ccm[i] = uint8_t(0x30 + i);
    check(c.writeMemory(0x10000000u, ccm, uint32_t(sizeof(ccm)), bbipc::kStOk,
                        "WRITE_MEMORY CCM"),
          "CCM write ok");
    uint8_t ccmBack[32] = {};
    check(c.readMemory(0x10000000u, ccmBack, uint32_t(sizeof(ccmBack)),
                       "READ_MEMORY CCM"),
          "CCM read ok");
    check(std::memcmp(ccmBack, ccm, sizeof(ccm)) == 0, "CCM read-back matches");

    // invalid addresses: both directions, no crash
    check(c.readMemoryExpect(0x90000000u, 4, bbipc::kStInvalidAddress,
                             "READ_MEMORY invalid address"),
          "invalid read -> InvalidAddress");
    uint32_t dummy = 0;
    check(c.writeMemory(0x90000000u, &dummy, 4, bbipc::kStInvalidAddress,
                        "WRITE_MEMORY invalid address"),
          "invalid write -> InvalidAddress");

    // flash: ordinary memory writes stay Unsupported (never a back door into
    // the programming path)
    uint32_t before = 0, after = 0;
    check(c.readMemory(ctx.a.code, &before, 4, "read flash word"), "flash read ok");
    check(c.writeMemory(ctx.a.code, &dummy, 4, bbipc::kStUnsupported,
                        "WRITE_MEMORY to flash"),
          "WRITE_MEMORY to flash -> Unsupported");
    check(c.readMemory(ctx.a.code, &after, 4, "re-read flash word"),
          "flash re-read ok");
    check(before == after, "flash bytes unchanged after the rejected write");
    check(c.writeMemory(0x00000000u, &dummy, 4, bbipc::kStUnsupported,
                        "WRITE_MEMORY to the flash alias"),
          "WRITE_MEMORY to the alias -> Unsupported");
    c.disconnect();
}

void phaseResetAndStep(TestClient& c, Ctx& ctx) {
    PhaseGuard ph("J/K reset-halt + exact single step");
    if (!openSession(c, ctx, "open session")) return;
    check(c.control(bbipc::kOpResetHalt, "RESET_HALT"), "RESET_HALT ok");
    c.drainStopEvents();  // reset publishes StopReason::Reset itself
    uint32_t st = 0, reason = 0, pc = 0;
    uint64_t cycles = 0;
    check(c.state(st, reason, pc, cycles, "GET_STATE after reset"),
          "GET_STATE ok");
    check(st == bbipc::kWireStateHalted, "halted after reset");
    check(reason == bbipc::kWireStopReset, "stop reason == Reset");
    check(pc == ctx.a.vecPc, "PC == vector reset handler");
    uint64_t sp = 0;
    check(c.readRegister(bbipc::kWireRegSP, sp, "read SP after reset"),
          "SP read ok");
    check(uint32_t(sp) == ctx.a.vecSp, "SP == vector initial SP");

    // one exact step: exactly one instruction executed -> PC +2, R1 = the
    // scratch address, cycles +1
    const uint64_t cyclesBefore = cycles;
    check(c.control(bbipc::kOpStep, "STEP"), "STEP ok");
    uint32_t pc2 = 0, st2 = 0, reason2 = 0;
    uint64_t cycles2 = 0;
    check(c.state(st2, reason2, pc2, cycles2, "GET_STATE after step"),
          "GET_STATE ok");
    check(reason2 == bbipc::kWireStopSingleStep, "stop reason == SingleStep");
    check(pc2 == ctx.a.reset + 2, "PC advanced by exactly one 16-bit instruction");
    check(cycles2 == cyclesBefore + 1, "virtual cycles advanced by exactly 1");
    uint64_t r1 = 0;
    check(c.readRegister(bbipc::kWireRegR1, r1, "read R1 after step"),
          "R1 read ok");
    check(uint32_t(r1) == ctx.a.magicAddr,
          "R1 == the scratch address (the instruction really executed)");
    c.disconnect();
}

void phaseBreakpoints(TestClient& c, Ctx& ctx) {
    PhaseGuard ph("L/M breakpoint hit + continue from breakpoint");
    if (!openSession(c, ctx, "open session")) return;
    check(c.control(bbipc::kOpResetHalt, "RESET_HALT"), "RESET_HALT ok");
    c.drainStopEvents();  // RESET_HALT publishes StopReason::Reset itself
    check(c.oneU32(bbipc::kOpAddBreakpoint, ctx.a.code,
                   "ADD_BREAKPOINT"),
          "ADD_BREAKPOINT ok");
    check(c.control(bbipc::kOpResume, "RESUME"), "RESUME ok");
    TestClient::Packet ev;
    check(c.waitEvent(bbipc::kEventTargetStopped, ev, 8000),
          "TARGET_STOPPED arrived");
    if (ev.ok) {
        bbipc::PayloadReader r(ev.payload.data(), ev.payload.size());
        uint32_t rsn = 0, evPc = 0;
        uint64_t cyc = 0;
        r.u32(rsn);
        r.u32(evPc);
        r.u64(cyc);
        check(r.ok(), "TARGET_STOPPED payload well formed");
        check(rsn == bbipc::kWireStopBreakpoint, "stop reason == Breakpoint");
        check(evPc == ctx.a.code, "event PC == breakpoint address");
        check(cyc != 0, "event carries virtual cycles");
    }
    uint32_t st = 0, reason = 0, pc = 0;
    uint64_t cycles = 0;
    check(c.state(st, reason, pc, cycles, "GET_STATE at breakpoint"),
          "GET_STATE ok");
    check(st == bbipc::kWireStateHalted, "halted at the breakpoint");
    check(pc == ctx.a.code, "PC sits ON the breakpoint (not past it)");

    // continue: a second breakpoint at the loop must be the NEXT stop, i.e.
    // the instruction at the first breakpoint must have been skipped once
    c.drainStopEvents(50);
    check(c.oneU32(bbipc::kOpAddBreakpoint, ctx.a.loopAddr,
                   "ADD_BREAKPOINT loop"),
          "second breakpoint added");
    check(c.control(bbipc::kOpResume, "RESUME (continue)"),
          "RESUME from breakpoint ok");
    TestClient::Packet ev2;
    check(c.waitEvent(bbipc::kEventTargetStopped, ev2, 8000),
          "second TARGET_STOPPED arrived");
    if (ev2.ok) {
        bbipc::PayloadReader r(ev2.payload.data(), ev2.payload.size());
        uint32_t rsn = 0, evPc = 0;
        uint64_t cyc = 0;
        r.u32(rsn);
        r.u32(evPc);
        r.u64(cyc);
        check(rsn == bbipc::kWireStopBreakpoint,
              "second stop reason == Breakpoint");
        check(evPc == ctx.a.loopAddr,
              "stopped at the NEXT breakpoint (no immediate re-stop)");
    }
    check(c.noPayload(bbipc::kOpClearBreakpoints, "CLEAR_BREAKPOINTS"),
          "CLEAR_BREAKPOINTS ok");
    c.disconnect();
}

void phaseHaltEvent(TestClient& c, Ctx& ctx) {
    PhaseGuard ph("N HALT event + frozen virtual time");
    if (!openSession(c, ctx, "open session")) return;
    check(c.control(bbipc::kOpResetRun, "RESET_RUN"), "RESET_RUN ok");
    c.drainStopEvents(100);  // RESET_RUN publishes StopReason::Reset itself
    QThread::msleep(40);
    check(c.control(bbipc::kOpHalt, "HALT"), "HALT ok");
    TestClient::Packet ev;
    check(c.waitEvent(bbipc::kEventTargetStopped, ev, 5000),
          "TARGET_STOPPED (UserHalt) arrived");
    if (ev.ok) {
        bbipc::PayloadReader r(ev.payload.data(), ev.payload.size());
        uint32_t rsn = 0, evPc = 0;
        uint64_t cyc = 0;
        r.u32(rsn);
        r.u32(evPc);
        r.u64(cyc);
        check(rsn == bbipc::kWireStopUserHalt, "stop reason == UserHalt");
    }
    uint32_t st = 0, reason = 0, pc = 0;
    uint64_t cyc1 = 0, cyc2 = 0;
    check(c.state(st, reason, pc, cyc1, "GET_STATE halted"),
          "GET_STATE ok");
    check(st == bbipc::kWireStateHalted, "state == Halted");
    QThread::msleep(50);
    check(c.state(st, reason, pc, cyc2, "GET_STATE halted again"),
          "GET_STATE ok");
    check(cyc1 == cyc2, "virtual cycles frozen while halted");
    c.disconnect();
}

void phaseDisconnectReconnect(TestClient& c, Ctx& ctx) {
    PhaseGuard ph("O disconnect cleanup + reconnect");
    if (!openSession(c, ctx, "open session")) return;
    const uint32_t firstSession = uint32_t(ctx.lastSessionId);
    // leave a breakpoint behind: the disconnect cleanup must clear it
    check(c.oneU32(bbipc::kOpAddBreakpoint, ctx.a.code, "ADD_BREAKPOINT"),
          "breakpoint added");
    c.disconnect();  // abrupt close -- the server must clean up on its own
    QThread::msleep(20);
    if (!openSession(c, ctx, "reconnect")) return;
    check(uint32_t(ctx.lastSessionId) != firstSession,
          "reconnect gets a NEW session id");
    // the reconnect only succeeds after the owner thread has run the session
    // cleanup (the worker waits for it), so the breakpoint set must be empty
    uint32_t st = 0, reason = 0, pc = 0;
    uint64_t cyc = 0;
    check(c.state(st, reason, pc, cyc, "GET_STATE after reconnect"),
          "GET_STATE ok");
    c.disconnect();
}

void phaseMalformed(TestClient& c, Ctx& ctx) {
    PhaseGuard ph("P malformed packet (bad magic)");
    if (!c.connect()) {
        check(false, "connect for malformed test");
        return;
    }
    uint8_t junk[bbipc::kHeaderSize];
    std::memset(junk, 0xEE, sizeof(junk));
    check(c.sendRaw(junk, sizeof(junk)), "send malformed header");
    TestClient::Packet p;
    const bool got = c.readPacket(p, 3000);
    check(got && p.kind == bbipc::kPacketResponse &&
              p.status == bbipc::kStProtocolError,
          "ProtocolError response then disconnect");
    // the session must be closed: a further read fails
    TestClient::Packet p2;
    check(!c.readPacket(p2, 1500), "connection closed after the bad packet");
    c.disconnect();
    (void)ctx;
}

void phaseWrongVersion(TestClient& c, Ctx& ctx) {
    PhaseGuard ph("Q wrong major version rejected");
    if (!c.connect()) {
        check(false, "connect for version test");
        return;
    }
    TestClient::Packet p;
    const bool got = c.req(bbipc::kOpHello, ([] {
                               std::vector<uint8_t> pl;
                               bbipc::PayloadWriter w(pl);
                               w.u16(2);  // wrong major
                               w.u16(0);
                               w.u32(1);
                               w.u32(bbipc::kClientTypeTest);
                               return pl;
                           })(),
                           bbipc::kStProtocolError, p,
                           "HELLO with major version 2 -> ProtocolError");
    check(got, "wrong major version rejected");
    // no debug command may execute on such a session
    TestClient::Packet step;
    const uint32_t id = c.nextRequestId();
    c.sendRequest(bbipc::kOpStep, {}, id);
    check(!c.waitResponse(id, step, 800), "STEP does not execute after the "
                                          "rejected handshake");
    c.disconnect();
    (void)ctx;
}

void phaseOversizePayload(TestClient& c, Ctx& ctx) {
    PhaseGuard ph("R oversized payload rejected without allocation");
    if (!c.connect()) {
        check(false, "connect for payload test");
        return;
    }
    bbipc::PacketHeader h;
    h.packetKind = bbipc::kPacketRequest;
    h.opcode = bbipc::kOpReadMemory;
    h.requestId = 42;
    h.payloadSize = bbipc::kMaxPayload + 1;  // hard limit + 1
    uint8_t raw[bbipc::kHeaderSize];
    bbipc::encodeHeader(raw, h);
    check(c.sendRaw(raw, sizeof(raw)), "send oversized header");
    TestClient::Packet p;
    const bool got = c.readPacket(p, 3000);
    check(got && p.kind == bbipc::kPacketResponse &&
              p.status == bbipc::kStProtocolError && p.requestId == 42,
          "oversized payload -> ProtocolError");
    c.disconnect();
    (void)ctx;
}

// Request timeout: with the owner loop deliberately stalled, a normal command
// must come back as Timeout (2 s budget), the session must stay usable and the
// abandoned request must NEVER produce a late (stale) response.
void phaseTimeout(TestClient& c, Ctx& ctx, std::atomic<int>& stallOwnerMs) {
    PhaseGuard ph("R2 request timeout + session recovery");
    if (!openSession(c, ctx, "open session")) return;
    stallOwnerMs.store(2500);
    QThread::msleep(80);  // let the owner loop enter the stall
    const uint32_t stalledId = c.nextRequestId();
    check(c.sendRequest(bbipc::kOpGetState, {}, stalledId), "send while stalled");
    QElapsedTimer t;
    t.start();
    TestClient::Packet p;
    const bool got = c.waitResponse(stalledId, p, 6000);
    const qint64 ms = t.elapsed();
    check(got && p.status == bbipc::kStTimeout,
          "stalled command answered with Timeout");
    if (!got || p.status != bbipc::kStTimeout) {
        say("  got=%d status=%u size=%u (expected Timeout=%u)\n", int(got),
            p.status, unsigned(p.payload.size()), unsigned(bbipc::kStTimeout));
    }
    check(ms >= 1800 && ms < 4500, "the 2 s response budget is honoured");
    {
        std::lock_guard<std::mutex> lk(g_perf);
        say("  [perf] stalled request -> Timeout after %lld ms\n",
            (long long)ms);
    }
    QThread::msleep(1300);  // let the stall finish and the owner resume
    uint32_t st = 0, reason = 0, pc = 0;
    uint64_t cyc = 0;
    check(c.state(st, reason, pc, cyc, "GET_STATE after the stall"),
          "the session recovers after a timeout");
    TestClient::Packet stale;
    check(!c.waitResponse(stalledId, stale, 200),
          "no stale response for the abandoned request");
    c.disconnect();
}

void phaseRequestIdStress(TestClient& c, Ctx& ctx) {
    PhaseGuard ph("S 10000x READ_REGISTER (requestId correspondence)");
    if (!openSession(c, ctx, "open session")) return;
    // the target is halted here; take the stable PC as the reference
    uint64_t pcRef = 0;
    if (!c.readRegister(bbipc::kWireRegPC, pcRef, "phase S reference PC")) {
        c.disconnect();
        return;
    }
    int bad = 0;
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < 10000; i++) {
        const uint32_t id = c.nextRequestId();
        if (!c.sendRequest(bbipc::kOpReadRegister, u32Payload(bbipc::kWireRegPC),
                           id)) {
            ++bad;
            break;
        }
        TestClient::Packet p;
        if (!c.waitResponse(id, p) || p.requestId != id ||
            p.status != bbipc::kStOk || p.payload.size() != 8) {
            ++bad;
            if (bad < 3) {
                say("  request %d failed (status=%u, err=%u)\n", i, p.status,
                    c.lastIoError());
            }
            continue;
        }
        uint64_t v = 0;
        bbipc::PayloadReader r(p.payload.data(), p.payload.size());
        r.u64(v);
        if (uint32_t(v) != uint32_t(pcRef)) ++bad;
    }
    check(bad == 0, "10000 register reads with matching requestIds");
    const double avgUs = double(t.elapsed()) * 1000.0 / 10000.0;
    {
        std::lock_guard<std::mutex> lk(g_perf);
        say("  [perf] READ_REGISTER avg RTT %.1f us (10000 rounds)\n", avgUs);
    }
    c.disconnect();
}

void phaseMemoryStress(TestClient& c, Ctx& ctx) {
    PhaseGuard ph("T 1000x READ_MEMORY 1 KiB");
    if (!openSession(c, ctx, "open session")) return;
    // publish a known 1 KiB pattern first
    std::vector<uint8_t> pattern(1024);
    for (int i = 0; i < 1024; i++) pattern[size_t(i)] = uint8_t(i * 13 + 7);
    check(c.writeMemory(0x20001000u, pattern.data(), 1024, bbipc::kStOk,
                        "publish 1 KiB pattern"),
          "pattern write ok");
    int bad = 0;
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < 1000; i++) {
        std::vector<uint8_t> back(1024, 0);
        if (!c.readMemory(0x20001000u, back.data(), 1024, "1 KiB read") ||
            back != pattern) {
            ++bad;
        }
    }
    check(bad == 0, "1000 x 1 KiB reads with correct data");
    const double avgUs = double(t.elapsed()) * 1000.0 / 1000.0;
    {
        std::lock_guard<std::mutex> lk(g_perf);
        say("  [perf] READ_MEMORY 1 KiB avg RTT %.1f us (1000 rounds)\n", avgUs);
    }
    c.disconnect();
}

void phaseRunStress(TestClient& c, Ctx& ctx) {
    PhaseGuard ph("U running target + pipelined reads + HALT priority");
    if (!openSession(c, ctx, "open session")) return;
    check(c.control(bbipc::kOpResetRun, "RESET_RUN"), "RESET_RUN ok");
    QThread::msleep(20);
    // pipeline 64 memory reads, then a HALT: the control command must jump the
    // queue (spec 111/112) and must not wait for the whole burst
    std::vector<uint32_t> ids;
    for (int i = 0; i < 64; i++) {
        std::vector<uint8_t> pl;
        bbipc::PayloadWriter w(pl);
        w.u32(0x20001000u);
        w.u32(4096);
        const uint32_t id = c.nextRequestId();
        check(c.sendRequest(bbipc::kOpReadMemory, pl, id), "pipelined read send");
        ids.push_back(id);
    }
    const uint32_t haltId = c.nextRequestId();
    QElapsedTimer t;
    t.start();
    check(c.sendRequest(bbipc::kOpHalt, {}, haltId), "pipelined HALT send");
    int reads = 0;
    for (uint32_t id : ids) {
        TestClient::Packet p;
        if (c.waitResponse(id, p, 8000) && p.status == bbipc::kStOk &&
            p.payload.size() == 4096) {
            ++reads;
        }
    }
    TestClient::Packet haltResp;
    const bool haltOk = c.waitResponse(haltId, haltResp, 8000) &&
                        haltResp.status == bbipc::kStOk;
    const qint64 haltMs = t.elapsed();
    check(reads == 64, "all 64 pipelined 4 KiB reads answered");
    check(haltOk, "HALT answered");
    check(haltMs < 3000, "pipelined burst + HALT completed in a bounded time");
    {
        std::lock_guard<std::mutex> lk(g_perf);
        say("  [perf] 64 x 4 KiB pipelined reads + HALT: %lld ms\n",
            (long long)haltMs);
    }
    uint32_t st = 0, reason = 0, pc = 0;
    uint64_t cyc = 0;
    check(c.state(st, reason, pc, cyc, "GET_STATE after stress halt"),
          "GET_STATE ok");
    check(st == bbipc::kWireStateHalted, "target halted after the burst");
    c.disconnect();
}

// A -> B -> A programming over IPC, plus the error/abort semantics.
void phaseProgramOverIpc(TestClient& c, Ctx& ctx) {
    PhaseGuard ph("V virtual flash programming over IPC (A->B->A, abort, errors)");
    if (!openSession(c, ctx, "open session")) return;

    // current firmware is A
    check(runAndCheckOverIpc(c, ctx, ctx.a, "A executes before programming"),
          "A firmware runs");

    // ---- program B --------------------------------------------------------
    TestClient::Packet p;
    const uint32_t id = c.nextRequestId();
    std::vector<uint8_t> beginPl = u32Payload(0);
    QElapsedTimer t;
    t.start();
    check(c.sendRequest(bbipc::kOpProgramBegin, beginPl, id) &&
              c.waitResponse(id, p, 6000) && p.status == bbipc::kStOk,
          "PROGRAM_BEGIN ok");
    uint64_t token = 0;
    uint32_t fb = 0, fs = 0;
    if (p.ok) {
        bbipc::PayloadReader r(p.payload.data(), p.payload.size());
        r.u64(token);
        r.u32(fb);
        r.u32(fs);
        check(r.ok() && fb == 0x08000000u && fs == 131072u,
              "PROGRAM_BEGIN reports the flash range");
        check(token != 0 && token != 1, "program token is session-unique");
    }
    check(c.reqOk(bbipc::kOpProgramErase,
                  tokenU32U32(token, 0x08000000u, 131072u), p,
                  "PROGRAM_ERASE"),
          "PROGRAM_ERASE ok");
    QElapsedTimer tw;
    tw.start();
    for (uint32_t off = 0; off < ctx.imageB.size(); off += 65536u) {
        const uint32_t len = std::min<uint32_t>(
            65536u, uint32_t(ctx.imageB.size() - off));
        check(c.reqOk(bbipc::kOpProgramWrite,
                      programWritePayload(token, 0x08000000u + off,
                                          ctx.imageB.data() + off, len),
                      p, "PROGRAM_WRITE"),
              "PROGRAM_WRITE chunk ok");
    }
    const qint64 writeMs = tw.elapsed();
    {
        std::lock_guard<std::mutex> lk(g_perf);
        say("  [perf] PROGRAM_WRITE 128 KiB over IPC: %lld ms\n",
            (long long)writeMs);
    }
    QElapsedTimer tc;
    tc.start();
    check(c.reqOk(bbipc::kOpProgramEnd, u64Payload(token), p, "PROGRAM_END"),
          "PROGRAM_END ok");
    const qint64 commitMs = tc.elapsed();
    {
        std::lock_guard<std::mutex> lk(g_perf);
        say("  [perf] PROGRAM_END commit + reset: %lld ms\n", (long long)commitMs);
    }
    if (p.ok) {
        bbipc::PayloadReader r(p.payload.data(), p.payload.size());
        uint32_t sp = 0, pc = 0;
        r.u32(sp);
        r.u32(pc);
        check(r.ok() && sp == ctx.b.vecSp && pc == ctx.b.vecPc,
              "PROGRAM_END returns the NEW vector table SP/PC");
    }
    uint32_t st = 0, reason = 0, pc = 0;
    uint64_t cyc = 0;
    check(c.state(st, reason, pc, cyc, "GET_STATE after PROGRAM_END"),
          "GET_STATE ok");
    check(st == bbipc::kWireStateHalted, "PROGRAM_END leaves the target halted");
    check(reason == bbipc::kWireStopReset, "PROGRAM_END stop reason == Reset");
    check(pc == ctx.b.vecPc, "PC == B reset handler after PROGRAM_END");

    // stale TCG: the opcode at the same VA must now be B's, and running must
    // publish B's values (a stale A translation would write A's result)
    uint16_t opAfter = 0;
    check(c.readMemory(ctx.b.code, &opAfter, 2, "read opcode after program"),
          "opcode read ok");
    check(opAfter == ctx.b.opcode, "flash contains B's opcode");
    check(opAfter != ctx.a.opcode, "the opcode really changed");
    check(runAndCheckOverIpc(c, ctx, ctx.b, "B executes after programming (no stale TCG)"),
          "B firmware runs");
    uint32_t aliasSp = 0;
    check(c.readMemory(0x00000000u, &aliasSp, 4, "read alias SP"), "alias read");
    check(aliasSp == ctx.b.vecSp, "alias mirrors the new flash vector table");

    // ---- abort does not touch live flash --------------------------------
    check(c.reqOk(bbipc::kOpProgramBegin, beginPl, p, "PROGRAM_BEGIN (abort test)"),
          "second PROGRAM_BEGIN ok");
    uint64_t token2 = 0;
    if (p.ok) {
        bbipc::PayloadReader r(p.payload.data(), p.payload.size());
        r.u64(token2);
        r.u32(fb);
        r.u32(fs);
    }
    check(c.reqOk(bbipc::kOpProgramErase,
                  tokenU32U32(token2, 0x08000000u, 4096u), p,
                  "PROGRAM_ERASE (abort test)"),
          "erase for abort ok");
    check(c.reqOk(bbipc::kOpProgramWrite,
                  programWritePayload(token2, 0x08000000u, ctx.imageA.data(),
                                      1024),
                  p, "PROGRAM_WRITE half image (abort test)"),
          "half write for abort ok");
    check(c.reqOk(bbipc::kOpProgramAbort, u64Payload(token2), p,
                  "PROGRAM_ABORT"),
          "PROGRAM_ABORT ok");
    check(runAndCheckOverIpc(c, ctx, ctx.b, "abort did not pollute live flash"),
          "B still executes after abort");

    // ---- error semantics -------------------------------------------------
    check(c.req(bbipc::kOpProgramAbort, u64Payload(token2), bbipc::kStProgramNotActive,
                p, "PROGRAM_ABORT when idle -> ProgramNotActive"),
          "abort without transaction -> ProgramNotActive");
    check(c.req(bbipc::kOpProgramErase, tokenU32U32(0x1234, 0x08000000u, 16u),
                bbipc::kStProgramNotActive, p,
                "PROGRAM_ERASE when idle -> ProgramNotActive"),
          "erase without transaction -> ProgramNotActive");

    check(c.reqOk(bbipc::kOpProgramBegin, beginPl, p, "PROGRAM_BEGIN (error test)"),
          "BEGIN for error tests ok");
    uint64_t token3 = 0;
    if (p.ok) {
        bbipc::PayloadReader r(p.payload.data(), p.payload.size());
        r.u64(token3);
        r.u32(fb);
        r.u32(fs);
    }
    check(c.req(bbipc::kOpProgramBegin, beginPl, bbipc::kStProgramAlreadyActive,
                p, "second PROGRAM_BEGIN -> ProgramAlreadyActive"),
          "double BEGIN -> ProgramAlreadyActive");
    check(c.req(bbipc::kOpProgramErase, tokenU32U32(token3 + 1, 0x08000000u, 16u),
                bbipc::kStProgramTokenInvalid, p,
                "PROGRAM_ERASE with a wrong token"),
          "wrong token -> ProgramTokenInvalid");
    check(c.req(bbipc::kOpProgramErase, tokenU32U32(token3, 0x08000000u, 0u),
                bbipc::kStOk, p, "PROGRAM_ERASE size 0 = Ok no-op"),
          "erase size 0 -> Ok");
    check(c.req(bbipc::kOpProgramErase, tokenU32U32(token3, 0x08020000u, 16u),
                bbipc::kStProgramRangeInvalid, p, "ERASE past the flash end"),
          "erase past flash -> ProgramRangeInvalid");
    uint8_t blob[16] = {};
    check(c.req(bbipc::kOpProgramWrite,
                programWritePayload(token3, 0x00000000u, blob, 16),
                bbipc::kStProgramRangeInvalid, p, "WRITE to the alias"),
          "write to the alias -> ProgramRangeInvalid");
    check(c.req(bbipc::kOpProgramWrite,
                programWritePayload(token3, 0xFFFFFFF0u, blob, 16),
                bbipc::kStProgramRangeInvalid, p, "WRITE with overflowing address"),
          "write overflow -> ProgramRangeInvalid");
    check(c.req(bbipc::kOpProgramWrite,
                programWritePayload(token3, 0x20000000u, blob, 16),
                bbipc::kStProgramRangeInvalid, p, "WRITE into RAM"),
          "write into RAM -> ProgramRangeInvalid");
    check(c.reqOk(bbipc::kOpProgramAbort, u64Payload(token3), p,
                  "PROGRAM_ABORT (error test)"),
          "abort after errors ok");
    check(runAndCheckOverIpc(c, ctx, ctx.b, "B still executes after the error "
                                            "cases"),
          "B unchanged after the error cases");

    // ---- A -> B -> A: program A back -------------------------------------
    check(programOverIpc(c, ctx.imageA, "PROGRAM A back"), "program A back ok");
    check(runAndCheckOverIpc(c, ctx, ctx.a, "A executes again (B->A)"),
          "A firmware runs again");
    check(programOverIpc(c, ctx.imageB, "PROGRAM B again"), "program B again ok");
    check(runAndCheckOverIpc(c, ctx, ctx.b, "B executes again (A->B)"),
          "B firmware runs again");
    check(programOverIpc(c, ctx.imageA, "PROGRAM A final"), "program A final ok");
    check(runAndCheckOverIpc(c, ctx, ctx.a, "A executes (final)"),
          "A firmware runs (final)");

    c.disconnect();
}

// programs a whole image over IPC (BEGIN/ERASE/WRITE/END) with checks
bool programOverIpc(TestClient& c, const std::vector<uint8_t>& image,
                    const char* what) {
    TestClient::Packet p;
    if (!c.reqOk(bbipc::kOpProgramBegin, u32Payload(0), p, what)) return false;
    uint64_t token = 0;
    uint32_t fb = 0, fs = 0;
    bbipc::PayloadReader r(p.payload.data(), p.payload.size());
    if (!r.u64(token) || !r.u32(fb) || !r.u32(fs)) {
        check(false, what);
        return false;
    }
    if (!c.reqOk(bbipc::kOpProgramErase,
                 tokenU32U32(token, 0x08000000u, uint32_t(image.size())), p,
                 what)) {
        return false;
    }
    for (uint32_t off = 0; off < image.size(); off += 65536u) {
        const uint32_t len =
            std::min<uint32_t>(65536u, uint32_t(image.size() - off));
        if (!c.reqOk(bbipc::kOpProgramWrite,
                     programWritePayload(token, 0x08000000u + off,
                                         image.data() + off, len),
                     p, what)) {
            return false;
        }
    }
    return c.reqOk(bbipc::kOpProgramEnd, u64Payload(token), p, what);
}

void phaseDisconnectDuringProgram(TestClient& c, Ctx& ctx) {
    PhaseGuard ph("W disconnect during a program transaction");
    if (!openSession(c, ctx, "open session")) return;
    // start a transaction, modify staging, then vanish without an abort
    TestClient::Packet p;
    check(c.reqOk(bbipc::kOpProgramBegin, u32Payload(0), p, "PROGRAM_BEGIN"),
          "BEGIN ok");
    uint64_t token = 0;
    if (p.ok) {
        bbipc::PayloadReader r(p.payload.data(), p.payload.size());
        uint32_t fb = 0, fs = 0;
        r.u64(token);
        r.u32(fb);
        r.u32(fs);
    }
    check(c.reqOk(bbipc::kOpProgramErase,
                  tokenU32U32(token, 0x08000000u, 4096u), p, "ERASE"),
          "erase ok");
    check(c.reqOk(bbipc::kOpProgramWrite,
                  programWritePayload(token, 0x08000000u, ctx.imageA.data(),
                                      512),
                  p, "WRITE"),
          "half write ok");
    c.disconnect();  // abrupt: the server must abort the transaction
    QThread::msleep(20);
    if (!openSession(c, ctx, "reconnect after the aborted transaction")) return;
    uint32_t st = 0, reason = 0, pc = 0;
    uint64_t cyc = 0;
    check(c.state(st, reason, pc, cyc, "GET_STATE after program disconnect"),
          "GET_STATE ok");
    check(st == bbipc::kWireStateHalted, "target halted after the cleanup");
    check(runAndCheckOverIpc(c, ctx, ctx.a, "live flash unchanged after the "
                                            "abandoned transaction"),
          "A firmware unaffected by the abandoned transaction");
    c.disconnect();
}

// Compact connect/HELLO/register/memory/breakpoint/disconnect cycle used for
// the 20-round stability requirement (spec 126).
void smokeRound(TestClient& c, Ctx& ctx, int round, bool& ok) {
    ok = true;
    if (!c.connect()) {
        check(false, "smoke: connect");
        ok = false;
        return;
    }
    TestClient::Packet p;
    if (!c.hello(1, 0, bbipc::kClientTypeTest, p, "smoke: HELLO")) {
        ok = false;
        return;
    }
    uint32_t sid = 0;
    if (p.ok) {
        bbipc::PayloadReader r(p.payload.data(), p.payload.size());
        uint16_t maj = 0, min = 0;
        uint32_t pid = 0, flags = 0;
        r.u16(maj);
        r.u16(min);
        r.u32(pid);
        r.u32(sid);
        r.u32(flags);
        check(sid != 0 && sid != uint32_t(ctx.lastSessionId),
              "smoke: session id changes every connect");
        ctx.lastSessionId = sid;
    }
    if (!c.control(bbipc::kOpResetHalt, "smoke: RESET_HALT")) ok = false;
    c.drainStopEvents(200);

    uint64_t pc = 0;
    if (!c.readRegister(bbipc::kWireRegPC, pc, "smoke: read PC")) ok = false;
    check((uint32_t(pc) & ~1u) == ctx.a.vecPc, "smoke: PC == vector");
    if (!c.writeRegister(bbipc::kWireRegR3, 0x5A5A0000u + uint32_t(round),
                         "smoke: write R3")) {
        ok = false;
    }
    uint64_t r3 = 0;
    if (!c.readRegister(bbipc::kWireRegR3, r3, "smoke: read R3")) ok = false;
    check(uint32_t(r3) == 0x5A5A0000u + uint32_t(round), "smoke: R3 read-back");

    std::vector<uint8_t> blob(64);
    for (size_t i = 0; i < blob.size(); i++) {
        blob[i] = uint8_t(round + int(i));
    }
    if (!c.writeMemory(0x20000800u, blob.data(), 64, bbipc::kStOk,
                       "smoke: write memory")) {
        ok = false;
    }
    std::vector<uint8_t> back(64, 0);
    if (!c.readMemory(0x20000800u, back.data(), 64, "smoke: read memory")) {
        ok = false;
    }
    check(back == blob, "smoke: memory read-back");

    if (!c.oneU32(bbipc::kOpAddBreakpoint, ctx.a.code,
                  "smoke: add breakpoint")) {
        ok = false;
    }
    if (!c.control(bbipc::kOpResume, "smoke: RESUME")) ok = false;
    TestClient::Packet ev;
    if (!c.waitEvent(bbipc::kEventTargetStopped, ev, 8000)) {
        check(false, "smoke: TARGET_STOPPED (breakpoint)");
        ok = false;
    } else {
        bbipc::PayloadReader r(ev.payload.data(), ev.payload.size());
        uint32_t rsn = 0, evPc = 0;
        uint64_t cyc = 0;
        r.u32(rsn);
        r.u32(evPc);
        r.u64(cyc);
        check(rsn == bbipc::kWireStopBreakpoint && evPc == ctx.a.code,
              "smoke: breakpoint event content");
    }
    if (!c.noPayload(bbipc::kOpClearBreakpoints, "smoke: clear breakpoints")) {
        ok = false;
    }
    if (!c.control(bbipc::kOpHalt, "smoke: HALT")) ok = false;
    c.disconnect();
}

}  // namespace

// ===========================================================================
int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    qRegisterMetaType<SimSnapshot>("SimSnapshot");

    const QString fwDir =
        QString::fromLocal8Bit(argc > 1 ? argv[1] : "firmware/");
    const QString hexA = fwDir + "test_program_a/test_program_a.hex";
    const QString hexB = fwDir + "test_program_b/test_program_b.hex";
    // Per-run pipe name: two simulators / two tests must never collide.
    // The client uses the FULL Win32 path; the server accepts both forms.
    const std::string pipeName =
        "\\\\.\\pipe\\BlueBridgeSimulator.Debug." +
        std::to_string(GetCurrentProcessId()) + ".IPC";

    Simulator sim;
    SimulatorDebugTarget target(sim);
    SimulatorFlashProgrammer programmer(sim);
    DebugIpcServer::Options opts;
    opts.pipeName = pipeName;
    opts.trace = true;  // exercise --debug-ipc-trace in the selftest
    DebugIpcServer server(opts);
    server.setLogSink([&sim](const std::string& s) {
        sim.debugLog(QString::fromStdString(s));
        // surface the non-per-packet [ipc] lines (the trace lines would flood)
        if (s.rfind("[ipc]", 0) == 0 && s.find(" op=0x") == std::string::npos) {
            say("  %s\n", s.c_str());
        }
    });
    server.setAttachObserver([&sim](bool on) { sim.setDebuggerAttached(on); });
    // TARGET_STOPPED / TARGET_FAULTED come from the Simulator's StopInfo
    sim.setStopObserver([&server](const StopInfo& info, uint64_t cycles) {
        server.notifyTargetStopped(info, cycles);
    });

    say("debug_ipc_selftest: firmware dir %s\npipe %s\n",
        fwDir.toLocal8Bit().constData(), pipeName.c_str());

    Ctx ctx;
    ctx.pipeName = pipeName;
    {
        PhaseGuard ph("prepare (owner thread): layouts A/B + direct state");
        check(target.loadFirmware(hexA.toStdString()) == DebugStatus::Ok,
              "load test_program_a");
        ctx.a = captureLayoutOnOwner(target);
        check(ctx.a.valid, "test_program_a map readable");
        uint32_t magic = 0, result = 0;
        check(runProgramOnOwner(target, ctx.a, magic, result),
              "run test_program_a");
        check(magic == ctx.a.magicValue && result == ctx.a.resultValue,
              "test_program_a publishes its expected values");

        check(target.loadFirmware(hexB.toStdString()) == DebugStatus::Ok,
              "load test_program_b");
        ctx.b = captureLayoutOnOwner(target);
        check(ctx.b.valid, "test_program_b map readable");
        check(runProgramOnOwner(target, ctx.b, magic, result),
              "run test_program_b");
        check(magic == ctx.b.magicValue && result == ctx.b.resultValue,
              "test_program_b publishes its expected values");

        check(ctx.a.reset == ctx.b.reset && ctx.a.code == ctx.b.code,
              "A and B share the layout (same addresses)");
        check(ctx.a.opcode != ctx.b.opcode,
              "A/B opcodes differ at the same address");
        check(ctx.a.loopAddr == ctx.b.loopAddr, "A/B loop address identical");
        say("  layout: reset=0x%08X code=0x%08X loop=0x%08X opA=0x%04X "
            "opB=0x%04X magicA=0x%08X magicB=0x%08X resultA=0x%X resultB=0x%X\n",
            ctx.a.reset, ctx.a.code, ctx.a.loopAddr, ctx.a.opcode, ctx.b.opcode,
            ctx.a.magicValue, ctx.b.magicValue, ctx.a.resultValue,
            ctx.b.resultValue);

        check(target.loadFirmware(hexA.toStdString()) == DebugStatus::Ok,
              "re-load test_program_a for the IPC phases");
        check(target.resetHalt() == DebugStatus::Ok, "reset-halt");
        uint64_t v = 0;
        target.readRegister(DebugRegister::PC, v);
        ctx.directPc = uint32_t(v) & ~1u;
        target.readRegister(DebugRegister::SP, v);
        ctx.directSp = uint32_t(v);
        target.readRegister(DebugRegister::R0, v);
        ctx.directR0 = uint32_t(v);
        target.readRegister(DebugRegister::XPSR, v);
        ctx.directXpsr = uint32_t(v);
        check(ctx.directPc == ctx.a.vecPc && ctx.directSp == ctx.a.vecSp,
              "direct reset-halt matches the A vector table");
        say("  direct: pc=0x%08X sp=0x%08X r0=0x%08X xpsr=0x%08X\n",
            ctx.directPc, ctx.directSp, ctx.directR0, ctx.directXpsr);
    }

    bool imageOkA = false, imageOkB = false;
    ctx.imageA = buildImageFromHex(hexA, imageOkA);
    ctx.imageB = buildImageFromHex(hexB, imageOkB);
    check(imageOkA && imageOkB, "HexLoader parsed both program images");

    check(server.start(), "DebugIpcServer::start");
    QThread::msleep(30);  // let the pipe instance come up

    std::atomic<bool> clientDone{false};
    std::atomic<int> smokeRoundsOk{0};
    // test-only: pauses the owner loop so the request-timeout path can be
    // exercised (a real GUI would stall the same way while blocked)
    std::atomic<int> stallOwnerMs{0};
    std::thread clientThread([&] {
        TestClient c(pipeName);
        phaseHelloAndCaps(c, ctx);
        phaseState(c, ctx);
        phaseRegisters(c, ctx);
        phaseMemory(c, ctx);
        phaseResetAndStep(c, ctx);
        phaseBreakpoints(c, ctx);
        phaseHaltEvent(c, ctx);
        phaseDisconnectReconnect(c, ctx);
        phaseMalformed(c, ctx);
        phaseWrongVersion(c, ctx);
        phaseOversizePayload(c, ctx);
        phaseTimeout(c, ctx, stallOwnerMs);
        phaseRequestIdStress(c, ctx);
        phaseMemoryStress(c, ctx);
        phaseRunStress(c, ctx);
        phaseProgramOverIpc(c, ctx);
        phaseDisconnectDuringProgram(c, ctx);

        // 20 stability rounds (round 1 was the sequence above)
        PhaseGuard ph("20 rounds stability (connect/disconnect/regs/mem/bp)");
        int passed = 0;
        for (int round = 1; round <= 20; round++) {
            bool ok = false;
            smokeRound(c, ctx, round, ok);
            if (ok) ++passed;
            say("  round %2d: %s\n", round, ok ? "PASS" : "FAIL");
        }
        check(passed == 20, "20/20 stability rounds");
        smokeRoundsOk.store(passed);
        clientDone.store(true);
    });

    // ---- owner loop: pump debug commands + run the target -----------------
    while (!clientDone.load()) {
        const int stall = stallOwnerMs.exchange(0);
        if (stall > 0) QThread::msleep(unsigned(stall));  // timeout test only
        server.processPendingCommands(target, programmer);
        if (sim.isRunning()) {
            QMetaObject::invokeMethod(&sim, "runTick", Qt::DirectConnection);
        } else {
            server.waitForCommands(2);
        }
    }
    clientThread.join();
    server.stop();

    // ---- owner-thread post conditions ------------------------------------
    {
        PhaseGuard ph("Z post conditions (owner thread)");
        check(sim.breakpoints().empty(),
              "disconnect cleanup cleared the debugger breakpoints");
        check(!programmer.programActive(),
              "no program transaction leaked past the sessions");
        check(server.sessionsServed() >= 21,
              "server served every session (>= 21 connects)");
        heapCheck("end of run");
        say("  sessions served: %llu, smoke rounds passed: %d\n",
            (unsigned long long)server.sessionsServed(),
            smokeRoundsOk.load());
    }

    say("\ntotal failures: %d\n", g_failures.load());
    return g_failures.load() == 0 ? 0 : 1;
}