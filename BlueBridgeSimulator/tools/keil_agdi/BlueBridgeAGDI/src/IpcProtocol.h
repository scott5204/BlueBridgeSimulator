#pragma once
/*
 * BlueBridge Debug IPC client -- hand-written wire codec (stage 7-2B.2).
 *
 * Layout authority: docs/debug_ipc_protocol.md v1.0 (24-byte little-endian
 * header, hand-encoded fields, payload layouts of section 6). Reuses only the
 * constants of DebugIpcWire.h.
 */

#include "IpcTypes.h"

namespace bbx {

struct PacketHeader {
    uint32_t magic = bbipc::kMagic;
    uint16_t versionMajor = bbipc::kVersionMajor;
    uint16_t versionMinor = bbipc::kVersionMinor;
    uint16_t kind = bbipc::kPacketRequest;
    uint16_t opcode = 0;
    uint32_t requestId = 0;
    uint32_t payloadSize = 0;
    uint32_t status = 0;
};

std::vector<uint8_t> encodeHeader(const PacketHeader &h);
bool decodeHeader(const uint8_t *p, PacketHeader &h);   // exactly 24 bytes

class PayloadWriter {
public:
    explicit PayloadWriter(std::vector<uint8_t> &buf) : buf_(buf) {}
    void u16(uint16_t v);
    void u32(uint32_t v);
    void u64(uint64_t v);
    void bytes(const void *p, size_t n);
    void str(const std::string &s);   // u16 length + raw bytes

private:
    std::vector<uint8_t> &buf_;
};

class PayloadReader {
public:
    PayloadReader(const uint8_t *p, size_t n) : p_(p), n_(n) {}
    bool u16(uint16_t &v);
    bool u32(uint32_t &v);
    bool u64(uint64_t &v);
    bool bytes(void *out, size_t n);
    bool str(std::string &s);
    bool ok() const { return ok_; }
    bool atEnd() const { return off_ == n_; }
    size_t remaining() const { return n_ - off_; }
    size_t offset() const { return off_; }

private:
    const uint8_t *p_;
    size_t n_;
    size_t off_ = 0;
    bool ok_ = true;
};

/* ---- payload builders ---------------------------------------------------- */
std::vector<uint8_t> buildHello(uint16_t major, uint16_t minor, uint32_t pid,
                                uint32_t clientType);
std::vector<uint8_t> buildU32(uint32_t v);            // READ_REGISTER(...)
std::vector<uint8_t> buildU64(uint64_t v);            // PING, PROGRAM_END
std::vector<uint8_t> buildWriteRegister(uint32_t id, uint64_t v);
std::vector<uint8_t> buildReadRegisters(const std::vector<uint32_t> &ids);
std::vector<uint8_t> buildWriteRegisters(const std::vector<RegisterValue> &in);
std::vector<uint8_t> buildReadMemory(uint32_t address, uint32_t length);
std::vector<uint8_t> buildWriteMemory(uint32_t address, const uint8_t *data,
                                      size_t length);
std::vector<uint8_t> buildProgramErase(uint64_t token, uint32_t address,
                                       uint32_t size);
std::vector<uint8_t> buildProgramWrite(uint64_t token, uint32_t address,
                                       const uint8_t *data, size_t length);

/* ---- payload parsers (return false on malformed input) -------------------- */
bool parseHello(const std::vector<uint8_t> &p, HelloInfo &out);
bool parseCapabilities(const std::vector<uint8_t> &p, TargetCaps &out);
bool parseState(const std::vector<uint8_t> &p, TargetState &out);
bool parseStopInfo(const std::vector<uint8_t> &p, uint32_t &reason, uint32_t &pc);
bool parseU64(const std::vector<uint8_t> &p, uint64_t &v);
bool parseRegisterBatch(const std::vector<uint8_t> &p,
                        std::vector<RegisterValue> &out);
bool parseWriteRegisterBatch(const std::vector<uint8_t> &p,
                             std::vector<RegisterValue> &out);
bool parseProgramBegin(const std::vector<uint8_t> &p, uint64_t &token,
                       uint32_t &flashBase, uint32_t &flashSize);
bool parseProgramEnd(const std::vector<uint8_t> &p, uint32_t &initialSp,
                     uint32_t &resetPc);
bool parseEventPayload(const std::vector<uint8_t> &p, uint16_t opcode,
                       IpcEvent &out);

}  // namespace bbx