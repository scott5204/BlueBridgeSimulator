#include "debug/ipc/DebugIpcCodec.h"

namespace bbipc {

// ---------------------------------------------------------------------------
// header
// ---------------------------------------------------------------------------
void encodeHeader(uint8_t out[kHeaderSize], const PacketHeader& h) {
    writeLe32(out + 0, kMagic);
    writeLe16(out + 4, h.versionMajor);
    writeLe16(out + 6, h.versionMinor);
    writeLe16(out + 8, h.packetKind);
    writeLe16(out + 10, h.opcode);
    writeLe32(out + 12, h.requestId);
    writeLe32(out + 16, h.payloadSize);
    writeLe32(out + 20, h.status);
}

bool decodeHeader(const uint8_t in[kHeaderSize], PacketHeader& out) {
    if (readLe32(in + 0) != kMagic) return false;
    out.versionMajor = readLe16(in + 4);
    out.versionMinor = readLe16(in + 6);
    out.packetKind = readLe16(in + 8);
    out.opcode = readLe16(in + 10);
    out.requestId = readLe32(in + 12);
    out.payloadSize = readLe32(in + 16);
    out.status = readLe32(in + 20);
    // packetKind is the only structurally required field beyond the magic; an
    // unknown kind is handled by the caller (protocol error), not here.
    return true;
}

// ---------------------------------------------------------------------------
// payload reader
// ---------------------------------------------------------------------------
bool PayloadReader::u8(uint8_t& out) {
    if (off_ + 1 > size_) { ok_ = false; return false; }
    out = data_[off_++];
    return true;
}

bool PayloadReader::u16(uint16_t& out) {
    if (off_ + 2 > size_) { ok_ = false; return false; }
    out = readLe16(data_ + off_);
    off_ += 2;
    return true;
}

bool PayloadReader::u32(uint32_t& out) {
    if (off_ + 4 > size_) { ok_ = false; return false; }
    out = readLe32(data_ + off_);
    off_ += 4;
    return true;
}

bool PayloadReader::u64(uint64_t& out) {
    if (off_ + 8 > size_) { ok_ = false; return false; }
    out = readLe64(data_ + off_);
    off_ += 8;
    return true;
}

bool PayloadReader::bytes(void* dst, size_t count) {
    if (count == 0) return true;
    if (!dst || off_ + count > size_) { ok_ = false; return false; }
    std::memcpy(dst, data_ + off_, count);
    off_ += count;
    return true;
}

const uint8_t* PayloadReader::raw(size_t count) {
    if (off_ + count > size_) { ok_ = false; return nullptr; }
    const uint8_t* p = data_ + off_;
    off_ += count;
    return p;
}

// ---------------------------------------------------------------------------
// payload writer
// ---------------------------------------------------------------------------
void PayloadWriter::u8(uint8_t v) { out_.push_back(v); }

void PayloadWriter::u16(uint16_t v) {
    uint8_t b[2];
    writeLe16(b, v);
    out_.insert(out_.end(), b, b + 2);
}

void PayloadWriter::u32(uint32_t v) {
    uint8_t b[4];
    writeLe32(b, v);
    out_.insert(out_.end(), b, b + 4);
}

void PayloadWriter::u64(uint64_t v) {
    uint8_t b[8];
    writeLe64(b, v);
    out_.insert(out_.end(), b, b + 8);
}

void PayloadWriter::bytes(const void* data, size_t count) {
    if (count == 0) return;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    out_.insert(out_.end(), p, p + count);
}

}  // namespace bbipc