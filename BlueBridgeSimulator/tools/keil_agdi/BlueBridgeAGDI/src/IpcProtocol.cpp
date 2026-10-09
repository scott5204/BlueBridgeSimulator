/*
 * BlueBridge Debug IPC client -- hand-written wire codec + logging hook.
 * No host struct ever crosses the pipe (7-2B.2 section 8).
 */

#include "IpcProtocol.h"

#include <Windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

bbx::LogFn g_logFn = NULL;

size_t StrLen(const char *s) {
    size_t n = 0;
    if (s) {
        while (s[n]) ++n;
    }
    return n;
}

}  // namespace

namespace bbx {

/* --------------------------------------------------------------------------
 * logging
 * ------------------------------------------------------------------------ */

void SetLogFn(LogFn fn) { g_logFn = fn; }

std::string ToUtf8(const wchar_t *w) {
    std::string out;
    if (w == NULL || w[0] == L'\0') return out;
    const int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (need <= 1) return out;
    out.resize((size_t)need - 1);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], need, NULL, NULL);
    return out;
}

void Logf(const char *fmt, ...) {
    if (!g_logFn) return;
    char body[768];
    va_list ap;
    va_start(ap, fmt);
    const int n = _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) {
        /* e.g. a %ls argument outside the C locale: keep the line visible
         * instead of silently dropping the whole message (use ToUtf8). */
        strncpy_s(body, sizeof(body), "<argument conversion failed>", _TRUNCATE);
    }

    char line[832];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[IPC] %s\r\n", body);
    g_logFn(line);
}

const char *registerName(uint32_t id) {
    static char buf[24];
    if (id <= bbipc::kWireRegR12) {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "R%u", (unsigned)id);
        return buf;
    }
    switch (id) {
        case bbipc::kWireRegSP: return "SP";
        case bbipc::kWireRegLR: return "LR";
        case bbipc::kWireRegPC: return "PC";
        case bbipc::kWireRegXPSR: return "xPSR";
        case bbipc::kWireRegMSP: return "MSP";
        case bbipc::kWireRegPSP: return "PSP";
        case bbipc::kWireRegPRIMASK: return "PRIMASK";
        case bbipc::kWireRegBASEPRI: return "BASEPRI";
        case bbipc::kWireRegFAULTMASK: return "FAULTMASK";
        case bbipc::kWireRegCONTROL: return "CONTROL";
        case bbipc::kWireRegFPSCR: return "FPSCR";
        default: break;
    }
    if (id >= bbipc::kWireRegS0 && id <= bbipc::kWireRegS31) {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "S%u",
                    (unsigned)(id - bbipc::kWireRegS0));
        return buf;
    }
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "reg(0x%08lX)", (unsigned long)id);
    return buf;
}

/* --------------------------------------------------------------------------
 * header
 * ------------------------------------------------------------------------ */

void AppendLe16(std::vector<uint8_t> &b, uint16_t v) {
    b.push_back((uint8_t)(v & 0xFF));
    b.push_back((uint8_t)((v >> 8) & 0xFF));
}

void AppendLe32(std::vector<uint8_t> &b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}

void AppendLe64(std::vector<uint8_t> &b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}

uint16_t ReadLe16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t ReadLe32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

uint64_t ReadLe64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

std::vector<uint8_t> encodeHeader(const PacketHeader &h) {
    std::vector<uint8_t> b;
    b.reserve(bbipc::kHeaderSize);
    AppendLe32(b, h.magic);
    AppendLe16(b, h.versionMajor);
    AppendLe16(b, h.versionMinor);
    AppendLe16(b, h.kind);
    AppendLe16(b, h.opcode);
    AppendLe32(b, h.requestId);
    AppendLe32(b, h.payloadSize);
    AppendLe32(b, h.status);
    return b;
}

bool decodeHeader(const uint8_t *p, PacketHeader &h) {
    h.magic = ReadLe32(p + 0);
    h.versionMajor = ReadLe16(p + 4);
    h.versionMinor = ReadLe16(p + 6);
    h.kind = ReadLe16(p + 8);
    h.opcode = ReadLe16(p + 10);
    h.requestId = ReadLe32(p + 12);
    h.payloadSize = ReadLe32(p + 16);
    h.status = ReadLe32(p + 20);
    return true;
}

/* --------------------------------------------------------------------------
 * payload writer / reader
 * ------------------------------------------------------------------------ */

void PayloadWriter::u16(uint16_t v) { AppendLe16(buf_, v); }
void PayloadWriter::u32(uint32_t v) { AppendLe32(buf_, v); }
void PayloadWriter::u64(uint64_t v) { AppendLe64(buf_, v); }

void PayloadWriter::bytes(const void *p, size_t n) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    buf_.insert(buf_.end(), b, b + n);
}

void PayloadWriter::str(const std::string &s) {
    u16((uint16_t)s.size());
    bytes(s.data(), s.size());
}

bool PayloadReader::u16(uint16_t &v) {
    if (!ok_ || off_ + 2 > n_) { ok_ = false; return false; }
    v = ReadLe16(p_ + off_);
    off_ += 2;
    return true;
}

bool PayloadReader::u32(uint32_t &v) {
    if (!ok_ || off_ + 4 > n_) { ok_ = false; return false; }
    v = ReadLe32(p_ + off_);
    off_ += 4;
    return true;
}

bool PayloadReader::u64(uint64_t &v) {
    if (!ok_ || off_ + 8 > n_) { ok_ = false; return false; }
    v = ReadLe64(p_ + off_);
    off_ += 8;
    return true;
}

bool PayloadReader::bytes(void *out, size_t n) {
    if (!ok_ || off_ + n > n_) { ok_ = false; return false; }
    const uint8_t *b = p_ + off_;
    uint8_t *o = static_cast<uint8_t *>(out);
    for (size_t i = 0; i < n; ++i) o[i] = b[i];
    off_ += n;
    return true;
}

bool PayloadReader::str(std::string &s) {
    uint16_t len = 0;
    if (!u16(len)) return false;
    if (off_ + len > n_) { ok_ = false; return false; }
    s.assign(reinterpret_cast<const char *>(p_ + off_), len);
    off_ += len;
    return true;
}

/* --------------------------------------------------------------------------
 * builders
 * ------------------------------------------------------------------------ */

std::vector<uint8_t> buildHello(uint16_t major, uint16_t minor, uint32_t pid,
                                uint32_t clientType) {
    std::vector<uint8_t> p;
    PayloadWriter w(p);
    w.u16(major);
    w.u16(minor);
    w.u32(pid);
    w.u32(clientType);
    return p;
}

std::vector<uint8_t> buildU32(uint32_t v) {
    std::vector<uint8_t> p;
    PayloadWriter(p).u32(v);
    return p;
}

std::vector<uint8_t> buildU64(uint64_t v) {
    std::vector<uint8_t> p;
    PayloadWriter(p).u64(v);
    return p;
}

std::vector<uint8_t> buildWriteRegister(uint32_t id, uint64_t v) {
    std::vector<uint8_t> p;
    PayloadWriter w(p);
    w.u32(id);
    w.u64(v);
    return p;
}

std::vector<uint8_t> buildReadRegisters(const std::vector<uint32_t> &ids) {
    std::vector<uint8_t> p;
    PayloadWriter w(p);
    w.u32((uint32_t)ids.size());
    for (size_t i = 0; i < ids.size(); ++i) w.u32(ids[i]);
    return p;
}

std::vector<uint8_t> buildWriteRegisters(const std::vector<RegisterValue> &in) {
    std::vector<uint8_t> p;
    PayloadWriter w(p);
    w.u32((uint32_t)in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        w.u32(in[i].id);
        w.u64(in[i].value);
    }
    return p;
}

std::vector<uint8_t> buildReadMemory(uint32_t address, uint32_t length) {
    std::vector<uint8_t> p;
    PayloadWriter w(p);
    w.u32(address);
    w.u32(length);
    return p;
}

std::vector<uint8_t> buildWriteMemory(uint32_t address, const uint8_t *data,
                                      size_t length) {
    std::vector<uint8_t> p;
    PayloadWriter w(p);
    w.u32(address);
    w.u32((uint32_t)length);
    w.bytes(data, length);
    return p;
}

std::vector<uint8_t> buildProgramErase(uint64_t token, uint32_t address,
                                       uint32_t size) {
    std::vector<uint8_t> p;
    PayloadWriter w(p);
    w.u64(token);
    w.u32(address);
    w.u32(size);
    return p;
}

std::vector<uint8_t> buildProgramWrite(uint64_t token, uint32_t address,
                                       const uint8_t *data, size_t length) {
    std::vector<uint8_t> p;
    PayloadWriter w(p);
    w.u64(token);
    w.u32(address);
    w.u32((uint32_t)length);
    w.bytes(data, length);
    return p;
}

/* --------------------------------------------------------------------------
 * parsers
 * ------------------------------------------------------------------------ */

bool parseHello(const std::vector<uint8_t> &p, HelloInfo &out) {
    PayloadReader r(p.data(), p.size());
    bool good = r.u16(out.serverMajor) && r.u16(out.serverMinor) &&
                r.u32(out.serverPid) && r.u32(out.sessionId) &&
                r.u32(out.featureFlags) && r.str(out.targetName) &&
                r.str(out.boardName) && r.str(out.architecture);
    return good && r.ok() && r.atEnd();
}

bool parseCapabilities(const std::vector<uint8_t> &p, TargetCaps &out) {
    PayloadReader r(p.data(), p.size());
    bool good = r.str(out.architecture) && r.u32(out.endian) &&
                r.u32(out.flags) && r.u32(out.flashBase) &&
                r.u32(out.flashSize) && r.u32(out.flashAliasBase) &&
                r.u32(out.flashAliasSize) && r.u32(out.sramBase) &&
                r.u32(out.sramSize) && r.u32(out.ccmBase) &&
                r.u32(out.ccmSize) && r.u32(out.maxMemoryTransfer) &&
                r.u32(out.maxProgramTransfer) && r.u32(out.maxPayload);
    return good && r.ok() && r.atEnd();
}

bool parseState(const std::vector<uint8_t> &p, TargetState &out) {
    PayloadReader r(p.data(), p.size());
    bool good = r.u32(out.state) && r.u32(out.stopReason) && r.u32(out.pc) &&
                r.u64(out.virtualCycles) && r.u32(out.firmwareLoaded);
    return good && r.ok() && r.atEnd();
}

bool parseStopInfo(const std::vector<uint8_t> &p, uint32_t &reason,
                   uint32_t &pc) {
    PayloadReader r(p.data(), p.size());
    bool good = r.u32(reason) && r.u32(pc);
    return good && r.ok() && r.atEnd();
}

bool parseU64(const std::vector<uint8_t> &p, uint64_t &v) {
    PayloadReader r(p.data(), p.size());
    bool good = r.u64(v);
    return good && r.ok() && r.atEnd();
}

bool parseRegisterBatch(const std::vector<uint8_t> &p,
                        std::vector<RegisterValue> &out) {
    PayloadReader r(p.data(), p.size());
    uint32_t count = 0;
    if (!r.u32(count)) return false;
    out.clear();
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        RegisterValue rv;
        if (!r.u32(rv.id) || !r.u64(rv.value) || !r.u32(rv.status)) return false;
        out.push_back(rv);
    }
    return r.ok() && r.atEnd();
}

bool parseWriteRegisterBatch(const std::vector<uint8_t> &p,
                             std::vector<RegisterValue> &out) {
    PayloadReader r(p.data(), p.size());
    uint32_t count = 0;
    if (!r.u32(count)) return false;
    out.clear();
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        RegisterValue rv;
        if (!r.u32(rv.id) || !r.u32(rv.status)) return false;
        out.push_back(rv);
    }
    return r.ok() && r.atEnd();
}

bool parseProgramBegin(const std::vector<uint8_t> &p, uint64_t &token,
                       uint32_t &flashBase, uint32_t &flashSize) {
    PayloadReader r(p.data(), p.size());
    bool good = r.u64(token) && r.u32(flashBase) && r.u32(flashSize);
    return good && r.ok() && r.atEnd();
}

bool parseProgramEnd(const std::vector<uint8_t> &p, uint32_t &initialSp,
                     uint32_t &resetPc) {
    PayloadReader r(p.data(), p.size());
    bool good = r.u32(initialSp) && r.u32(resetPc);
    return good && r.ok() && r.atEnd();
}

bool parseEventPayload(const std::vector<uint8_t> &p, uint16_t opcode,
                       IpcEvent &out) {
    PayloadReader r(p.data(), p.size());
    out.opcode = opcode;
    bool good = r.u32(out.stopReason) && r.u32(out.pc) && r.u64(out.cycles);
    return good && r.ok() && r.atEnd();
}

}  // namespace bbx