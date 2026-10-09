#pragma once

// ============================================================================
// BlueBridge Debug IPC -- wire protocol v1.0, byte-level definitions.
//
// This is the ONLY header a non-MinGW client (the future MSVC
// BlueBridgeAGDI.dll, or a test/CLI client) needs in order to speak the
// protocol. It therefore contains:
//   * fixed-width integers ONLY,
//   * flat enums with explicit wire values (never a host struct on the wire),
//   * no Qt, no unicorn, no Simulator/SoC headers, no STL containers.
//
// Wire rules (see docs/debug_ipc_protocol.md):
//   * every packet starts with a fixed 24-byte header, little-endian,
//     hand-encoded (never memcpy a host struct: MinGW and MSVC may disagree on
//     enum size, bool size, padding and alignment),
//   * requestId is chosen by the client and echoed by the server,
//   * a request that times out in the server's pipe worker is answered with
//     kStTimeout and must not be retried blindly (see the protocol doc).
// ============================================================================

#include <stdint.h>

namespace bbipc {

// 'B' 'B' 'G' 'D' -- BlueBridge Guest Debug (little-endian byte order).
enum : uint32_t { kMagic = 0x44474242u };

enum : uint16_t { kVersionMajor = 1, kVersionMinor = 0 };

// ---- protocol limits -------------------------------------------------------
enum : uint32_t {
    // Hard cap on any payload; a larger payloadSize is a protocol error and
    // must be rejected WITHOUT allocating (spec: no huge allocations).
    kMaxPayload = 1024u * 1024u,           // 1 MiB
    kMaxMemoryTransfer = 64u * 1024u,      // READ_MEMORY / WRITE_MEMORY
    kMaxProgramTransfer = 64u * 1024u,     // PROGRAM_WRITE
    kHeaderSize = 24,
    // Pipe-worker response timeouts (the owner loop must answer within this).
    kRequestTimeoutMs = 2000,
    kProgramEndTimeoutMs = 10000,
    // The client's HELLO clientType values.
    kClientTypeTest = 1,
    kClientTypeKeilAgdi = 2,               // reserved for stage 7-2B
};

// ---- packet kinds ----------------------------------------------------------
enum PacketKind : uint16_t {
    kPacketRequest = 1,
    kPacketResponse = 2,
    kPacketEvent = 3,
};

// ---- opcodes --------------------------------------------------------------
// 0x0000-0x00FF core, 0x0100 register, 0x0200 memory, 0x0300 breakpoint,
// 0x0400 programming, >=0x8000 events.
enum WireOpcode : uint16_t {
    kOpHello = 0x0001,
    kOpGetCapabilities = 0x0002,
    kOpGetState = 0x0003,
    kOpGetStopInfo = 0x0004,
    kOpHalt = 0x0005,
    kOpResume = 0x0006,
    kOpStep = 0x0007,
    kOpResetHalt = 0x0008,
    kOpResetRun = 0x0009,
    kOpPing = 0x000A,

    kOpReadRegister = 0x0100,
    kOpWriteRegister = 0x0101,
    kOpReadRegisters = 0x0102,
    kOpWriteRegisters = 0x0103,

    kOpReadMemory = 0x0200,
    kOpWriteMemory = 0x0201,

    kOpAddBreakpoint = 0x0300,
    kOpRemoveBreakpoint = 0x0301,
    kOpClearBreakpoints = 0x0302,

    kOpProgramBegin = 0x0400,
    kOpProgramErase = 0x0401,
    kOpProgramWrite = 0x0402,
    kOpProgramEnd = 0x0403,
    kOpProgramAbort = 0x0404,

    // async events (server -> client)
    kEventTargetStopped = 0x8001,
    kEventTargetFaulted = 0x8002,
    kEventProgramProgress = 0x8003,   // reserved, not emitted in v1.0
};

// ---- status ---------------------------------------------------------------
// Stable protocol enum: never send the internal DebugStatus integer.
enum WireStatus : uint32_t {
    kStOk = 0,
    kStInvalidCommand = 1,
    kStInvalidState = 2,
    kStInvalidRegister = 3,
    kStInvalidAddress = 4,
    kStUnsupported = 5,
    kStBusy = 6,
    kStTimeout = 7,
    kStProtocolError = 8,
    kStTargetFault = 9,
    kStProgramNotActive = 10,
    kStProgramAlreadyActive = 11,
    kStProgramTokenInvalid = 12,
    kStProgramRangeInvalid = 13,
    kStInternalError = 14,
};

// ---- target state / stop reason (GET_STATE, TARGET_STOPPED) ---------------
enum WireTargetState : uint32_t {
    kWireStateHalted = 0,
    kWireStateRunning = 1,
    kWireStateReset = 2,
    kWireStateFault = 3,
};

enum WireStopReason : uint32_t {
    kWireStopNone = 0,
    kWireStopUserHalt = 1,
    kWireStopBreakpoint = 2,
    kWireStopSingleStep = 3,
    kWireStopReset = 4,
    kWireStopFault = 5,
};

// ---- register ids ---------------------------------------------------------
// Frozen: must never follow an internal enum reorder.
enum WireRegisterId : uint32_t {
    kWireRegR0 = 0,
    kWireRegR1 = 1,
    kWireRegR2 = 2,
    kWireRegR3 = 3,
    kWireRegR4 = 4,
    kWireRegR5 = 5,
    kWireRegR6 = 6,
    kWireRegR7 = 7,
    kWireRegR8 = 8,
    kWireRegR9 = 9,
    kWireRegR10 = 10,
    kWireRegR11 = 11,
    kWireRegR12 = 12,
    kWireRegSP = 13,
    kWireRegLR = 14,
    kWireRegPC = 15,
    kWireRegXPSR = 16,
    kWireRegMSP = 17,
    kWireRegPSP = 18,
    kWireRegPRIMASK = 19,
    kWireRegBASEPRI = 20,
    kWireRegFAULTMASK = 21,
    kWireRegCONTROL = 22,
    kWireRegS0 = 32,
    // S1..S31 = 33..63
    kWireRegS31 = 63,
    kWireRegFPSCR = 64,
    kWireRegInvalid = 0xFFFFFFFFu,
};

// ---- capability flags (GET_CAPABILITIES / HELLO featureFlags) -------------
enum CapabilityFlag : uint32_t {
    kCapExactSingleStep = 1u << 0,
    kCapExecutionBreakpoint = 1u << 1,
    kCapDataWatchpoint = 1u << 2,   // no (stage 7-2A)
    kCapFpu = 1u << 3,
    kCapFlashProgramming = 1u << 4,
    kCapAsyncStopEvent = 1u << 5,
    kCapBatchRegisterRead = 1u << 6,
};

enum WireEndian : uint32_t {
    kWireEndianLittle = 1,
};

}  // namespace bbipc