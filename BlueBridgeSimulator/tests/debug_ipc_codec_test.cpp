// ============================================================================
// Stage 7-2A: wire codec golden test.
//
// Pins the BYTE-LEVEL protocol so a future MSVC/AGDI implementation cannot
// drift silently: fixed 24-byte little-endian header, frozen enum values and
// frozen register-id mapping (spec 121/122). The payload reader/writer is
// checked for bounds safety as well.
// Exit code 0 = PASS.
// ============================================================================
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "debug/ipc/DebugIpcCodec.h"

// ---- frozen wire constants (compile-time) ---------------------------------
static_assert(bbipc::kMagic == 0x44474242u, "magic is frozen");
static_assert(bbipc::kVersionMajor == 1 && bbipc::kVersionMinor == 0,
              "protocol version is frozen");
static_assert(bbipc::kHeaderSize == 24, "the header is exactly 24 bytes");
static_assert(bbipc::kMaxPayload == 1048576u, "payload limit is frozen");
static_assert(bbipc::kMaxMemoryTransfer == 65536u, "memory limit is frozen");
static_assert(bbipc::kMaxProgramTransfer == 65536u, "program limit is frozen");
static_assert(bbipc::kPacketRequest == 1 && bbipc::kPacketResponse == 2 &&
                  bbipc::kPacketEvent == 3,
              "packet kinds are frozen");
static_assert(bbipc::kOpHello == 0x0001, "HELLO opcode is frozen");
static_assert(bbipc::kOpGetState == 0x0003, "GET_STATE opcode is frozen");
static_assert(bbipc::kOpReadRegister == 0x0100, "register opcodes are frozen");
static_assert(bbipc::kOpReadMemory == 0x0200, "memory opcodes are frozen");
static_assert(bbipc::kOpAddBreakpoint == 0x0300,
              "breakpoint opcodes are frozen");
static_assert(bbipc::kOpProgramBegin == 0x0400, "program opcodes are frozen");
static_assert(bbipc::kOpProgramEnd == 0x0403, "PROGRAM_END opcode is frozen");
static_assert(bbipc::kEventTargetStopped == 0x8001,
              "event opcodes are frozen");
static_assert(bbipc::kStOk == 0 && bbipc::kStInvalidCommand == 1 &&
                  bbipc::kStInvalidState == 2 &&
                  bbipc::kStInvalidRegister == 3 &&
                  bbipc::kStInvalidAddress == 4 &&
                  bbipc::kStUnsupported == 5 && bbipc::kStBusy == 6 &&
                  bbipc::kStTimeout == 7 && bbipc::kStProtocolError == 8 &&
                  bbipc::kStTargetFault == 9 &&
                  bbipc::kStProgramNotActive == 10 &&
                  bbipc::kStProgramAlreadyActive == 11 &&
                  bbipc::kStProgramTokenInvalid == 12 &&
                  bbipc::kStProgramRangeInvalid == 13 &&
                  bbipc::kStInternalError == 14,
              "wire status values are frozen");
static_assert(bbipc::kWireRegR12 == 12 && bbipc::kWireRegSP == 13 &&
                  bbipc::kWireRegPC == 15 && bbipc::kWireRegXPSR == 16 &&
                  bbipc::kWireRegCONTROL == 22 && bbipc::kWireRegS0 == 32 &&
                  bbipc::kWireRegS31 == 63 && bbipc::kWireRegFPSCR == 64,
              "register wire ids are frozen");

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}

}  // namespace

int main() {
    using namespace bbipc;

    // ---- golden header bytes -------------------------------------------------
    {
        PacketHeader h;
        h.versionMajor = 1;
        h.versionMinor = 0;
        h.packetKind = kPacketRequest;
        h.opcode = kOpProgramWrite;  // 0x0402
        h.requestId = 0x11223344u;
        h.payloadSize = 0x0102u;
        h.status = 0;
        uint8_t raw[kHeaderSize];
        std::memset(raw, 0xEE, sizeof(raw));
        encodeHeader(raw, h);

        const uint8_t expected[kHeaderSize] = {
            0x42, 0x42, 0x47, 0x44,  // 'B' 'B' 'G' 'D'
            0x01, 0x00,              // major 1
            0x00, 0x00,              // minor 0
            0x01, 0x00,              // packetKind = request
            0x02, 0x04,              // opcode 0x0402
            0x44, 0x33, 0x22, 0x11,  // requestId (LE)
            0x02, 0x01, 0x00, 0x00,  // payloadSize (LE)
            0x00, 0x00, 0x00, 0x00,  // status
        };
        check(std::memcmp(raw, expected, kHeaderSize) == 0,
              "encoded header matches the golden 24 bytes");

        PacketHeader back;
        check(decodeHeader(raw, back), "decodeHeader accepts the golden bytes");
        check(back.versionMajor == 1 && back.versionMinor == 0 &&
                  back.packetKind == kPacketRequest &&
                  back.opcode == kOpProgramWrite &&
                  back.requestId == 0x11223344u &&
                  back.payloadSize == 0x0102u && back.status == 0,
              "decoded header equals the encoded header");

        uint8_t bad[kHeaderSize];
        std::memcpy(bad, raw, kHeaderSize);
        bad[0] = 'X';
        PacketHeader junk;
        check(!decodeHeader(bad, junk), "bad magic is rejected");
    }

    // ---- event / response header round trip ---------------------------------
    {
        PacketHeader h;
        h.packetKind = kPacketEvent;
        h.opcode = kEventTargetStopped;
        h.requestId = 0;
        h.payloadSize = 16;
        h.status = kStOk;
        uint8_t raw[kHeaderSize];
        encodeHeader(raw, h);
        PacketHeader back;
        check(decodeHeader(raw, back) && back.packetKind == kPacketEvent &&
                  back.opcode == kEventTargetStopped && back.requestId == 0 &&
                  back.payloadSize == 16,
              "event header round trip");
    }

    // ---- payload writer / reader --------------------------------------------
    {
        std::vector<uint8_t> buf;
        PayloadWriter w(buf);
        w.u8(0xAB);
        w.u16(0x1234);
        w.u32(0xDEADBEEF);
        w.u64(0x0123456789ABCDEFull);
        const uint8_t blob[3] = {1, 2, 3};
        w.bytes(blob, 3);

        const uint8_t expected[] = {
            0xAB, 0x34, 0x12, 0xEF, 0xBE, 0xAD, 0xDE,
            0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01,
            0x01, 0x02, 0x03,
        };
        check(buf.size() == sizeof(expected) &&
                  std::memcmp(buf.data(), expected, sizeof(expected)) == 0,
              "payload writer emits little-endian bytes");

        PayloadReader r(buf.data(), buf.size());
        uint8_t a = 0;
        uint16_t b = 0;
        uint32_t c = 0;
        uint64_t d = 0;
        uint8_t out[3] = {};
        const bool ok = r.u8(a) && r.u16(b) && r.u32(c) && r.u64(d) &&
                        r.bytes(out, 3);
        check(ok && a == 0xAB && b == 0x1234 && c == 0xDEADBEEF &&
                  d == 0x0123456789ABCDEFull && out[0] == 1 && out[1] == 2 &&
                  out[2] == 3,
              "payload reader returns the written values");
        check(r.ok() && r.atEnd() && r.remaining() == 0,
              "reader consumed the whole buffer");

        // truncated payload: every accessor must fail instead of overrunning
        PayloadReader t(buf.data(), 3);
        uint32_t v = 0;
        check(!t.u32(v), "truncated u32 fails");
        check(!t.ok(), "the reader latches the failure");
        PayloadReader t2(buf.data(), 2);
        uint64_t v64 = 0;
        check(!t2.u64(v64), "truncated u64 fails");
        const uint8_t* raw = t2.raw(100);
        check(raw == nullptr, "raw() beyond the end returns nullptr");
        check(!t2.ok(), "raw() latches the failure too");

        // trailing bytes are visible (strict framing in the server)
        PayloadReader t3(buf.data(), buf.size() - 1);
        uint8_t x = 0;
        uint16_t y = 0;
        uint32_t z = 0;
        uint64_t q = 0;
        t3.u8(x);
        t3.u16(y);
        t3.u32(z);
        t3.u64(q);
        check(t3.ok() && !t3.atEnd(), "trailing bytes are reported");
    }

    std::printf("debug_ipc_codec_test: %s (%d failure(s))\n",
                g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}