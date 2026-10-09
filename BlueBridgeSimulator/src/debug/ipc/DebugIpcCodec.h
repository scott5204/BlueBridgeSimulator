#pragma once

// ============================================================================
// Debug IPC wire codec (MinGW side).
//
// Everything on the wire is hand-encoded little-endian. No host struct is ever
// memcpy'd onto the pipe and no struct layout is assumed on the way in (see
// DebugIpcWire.h). The server and the test client share THIS codec, but they
// exchange real bytes over a real Named Pipe -- the server's decoded command
// structs never bypass the wire (spec: test the framing, not the internals).
//
// Dependency-free except for the wire constants + <cstdint>/<vector>, so the
// same reader/writer helpers can be reused by a future MSVC AGDI codec.
// ============================================================================

#include <cstdint>
#include <cstring>
#include <vector>

#include "debug/ipc/DebugIpcWire.h"

namespace bbipc {

// ---- little-endian primitives ---------------------------------------------
inline uint16_t readLe16(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
inline uint32_t readLe32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}
inline uint64_t readLe64(const uint8_t* p) {
    return uint64_t(readLe32(p)) | (uint64_t(readLe32(p + 4)) << 32);
}
inline void writeLe16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
inline void writeLe32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}
inline void writeLe64(uint8_t* p, uint64_t v) {
    writeLe32(p, uint32_t(v));
    writeLe32(p + 4, uint32_t(v >> 32));
}

// ---- fixed 24-byte packet header ------------------------------------------
struct PacketHeader {
    uint16_t versionMajor = kVersionMajor;
    uint16_t versionMinor = kVersionMinor;
    uint16_t packetKind = kPacketRequest;
    uint16_t opcode = 0;
    uint32_t requestId = 0;
    uint32_t payloadSize = 0;
    uint32_t status = kStOk;   // 0 in requests (kind-dependent, see the doc)
};

// Encodes exactly kHeaderSize (24) bytes: magic, version, kind, opcode,
// requestId, payloadSize, status -- all little-endian.
void encodeHeader(uint8_t out[kHeaderSize], const PacketHeader& h);
// Validates the magic and the fixed field layout; returns false on a bad magic
// or a packet that is not a well-formed header (the caller must then reject the
// connection -- never read a payload it cannot trust).
bool decodeHeader(const uint8_t in[kHeaderSize], PacketHeader& out);

// ---- payload reading ------------------------------------------------------
// Bounds-checked sequential reader. Every accessor returns false and latches
// the failure when the buffer is too short; ok() reports the latched state, so
// a caller may chain reads and check once.
class PayloadReader {
public:
    PayloadReader(const uint8_t* data, size_t size)
        : data_(data), size_(size) {}

    bool u8(uint8_t& out);
    bool u16(uint16_t& out);
    bool u32(uint32_t& out);
    bool u64(uint64_t& out);
    // Reads @count raw bytes into @dst (may be null only when count == 0).
    bool bytes(void* dst, size_t count);
    // Returns a pointer into the buffer for @count bytes (no copy) or nullptr.
    const uint8_t* raw(size_t count);

    bool ok() const { return ok_; }
    size_t remaining() const { return size_ - off_; }
    bool atEnd() const { return off_ == size_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t off_ = 0;
    bool ok_ = true;
};

// ---- payload writing ------------------------------------------------------
class PayloadWriter {
public:
    explicit PayloadWriter(std::vector<uint8_t>& out) : out_(out) {}
    void u8(uint8_t v);
    void u16(uint16_t v);
    void u32(uint32_t v);
    void u64(uint64_t v);
    void bytes(const void* data, size_t count);
    void raw(const std::vector<uint8_t>& v) { bytes(v.data(), v.size()); }

private:
    std::vector<uint8_t>& out_;
};

}  // namespace bbipc