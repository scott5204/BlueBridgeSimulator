#pragma once
/*
 * BlueBridge Debug IPC client -- shared types.
 *
 * Used by BOTH BlueBridgeAGDI.dll and BlueBridgeAGDIProbe.exe (stage 7-2B.2
 * section 5: the probe and the DLL must reuse exactly the same IpcClient /
 * codec / wire constants, so an IPC problem can never be blamed on the AGDI
 * mapping and vice versa).
 *
 * Wire constants come from the simulator's own `DebugIpcWire.h` -- the protocol
 * doc explicitly allows a non-MinGW client to reuse that header. The codec
 * itself is hand-written (IpcProtocol.cpp); a host struct is NEVER memcpy'd
 * onto the wire (section 8).
 */

#include <stdint.h>

#include <string>
#include <vector>

#include "DebugIpcWire.h"

namespace bbx {

/* --------------------------------------------------------------------------
 * logging: the host (DLL -> file log, probe -> stdout) installs a sink
 * ------------------------------------------------------------------------ */
typedef void (*LogFn)(const char *line);
void SetLogFn(LogFn fn);
void Logf(const char *fmt, ...);   // "[IPC] ..." + CRLF, then the sink

/* Wide -> UTF-8 for log arguments. `%ls` must NOT be used for arguments that
 * can hold non-ASCII text: the C locale conversion fails, _vsnprintf_s returns
 * -1 and the whole message body is dropped (measured with a 中文 install path
 * and with localized FormatMessageW error text). Convert first, log with %s. */
std::string ToUtf8(const wchar_t *w);

/* --------------------------------------------------------------------------
 * client-side status codes (never sent on the wire; they only extend
 * bbipc::WireStatus for the caller)
 * ------------------------------------------------------------------------ */
enum : uint32_t {
    kStNotConnected = 0xFFFF0001u,   // no session at all
    kStConnectionLost = 0xFFFF0002u, // pipe broke / simulator gone
    kStWriteTimeout = 0xFFFF0003u,   // could not flush a request in time
    kStBadResponse = 0xFFFF0004u,    // framing/parse failure
    kStBadArgument = 0xFFFF0005u,
};

struct RequestResult {
    uint32_t status = kStNotConnected;
    std::vector<uint8_t> payload;
    bool ok() const { return status == bbipc::kStOk; }
    bool lost() const {
        return status == kStNotConnected || status == kStConnectionLost ||
               status == kStWriteTimeout;
    }
};

struct RegisterValue {
    uint32_t id = bbipc::kWireRegInvalid;
    uint64_t value = 0;
    uint32_t status = bbipc::kStOk;
};

struct HelloInfo {
    uint16_t serverMajor = 0;
    uint16_t serverMinor = 0;
    uint32_t serverPid = 0;
    uint32_t sessionId = 0;
    uint32_t featureFlags = 0;
    std::string targetName;
    std::string boardName;
    std::string architecture;
};

struct TargetCaps {
    std::string architecture;
    uint32_t endian = 0;
    uint32_t flags = 0;
    uint32_t flashBase = 0, flashSize = 0;
    uint32_t flashAliasBase = 0, flashAliasSize = 0;
    uint32_t sramBase = 0, sramSize = 0;
    uint32_t ccmBase = 0, ccmSize = 0;
    uint32_t maxMemoryTransfer = 0, maxProgramTransfer = 0, maxPayload = 0;
};

struct TargetState {
    uint32_t state = 0;       // bbipc::WireTargetState
    uint32_t stopReason = 0;  // bbipc::WireStopReason
    uint32_t pc = 0;
    uint32_t firmwareLoaded = 0;
    uint64_t virtualCycles = 0;
};

struct IpcEvent {
    uint16_t opcode = 0;      // kEventTargetStopped / kEventTargetFaulted
    uint32_t stopReason = 0;
    uint32_t pc = 0;
    uint64_t cycles = 0;
};

/* B.3: every received target event gets a local, monotonically increasing
 * sequence number. A run waiter records the sequence it saw BEFORE sending
 * RESUME and only accepts an event with a HIGHER sequence -- a stale
 * SingleStep/Reset event left over from an earlier operation can therefore
 * never complete a new run (spec sections 15-20 / 52-53). */
struct TargetEvent {
    uint64_t sequence = 0;
    uint16_t opcode = 0;      // kEventTargetStopped / kEventTargetFaulted
    uint32_t stopReason = 0;
    uint32_t pc = 0;
    uint64_t cycles = 0;
};

/* Outcome of waitForEventAfter(): why the wait ended. */
enum class EventWaitResult {
    Event,           // a NEW event arrived (out is filled)
    ConnectionLost,  // pipe broke / simulator gone / write failed
    Shutdown,        // disconnect() was called
    Timeout,         // only when a finite timeout was requested
};

/* printable register id (for logs / probe output) */
const char *registerName(uint32_t id);

}  // namespace bbx