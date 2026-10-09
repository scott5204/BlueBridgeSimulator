# BlueBridge Debug IPC — protocol v1.0

> Stage **7-2A** (Debug IPC + Virtual Flash Programming).
> Audience: the next stage's **BlueBridgeAGDI.dll** (MSVC), CLI tools and test
> clients. Everything here is fixed; re-implement the codec from this document
> (or reuse `src/debug/ipc/DebugIpcWire.h`, which is C-compatible).

---

## 1. Transport

* **Windows Named Pipe**, byte mode, duplex, **one instance** per simulator.
  A second simultaneous client gets `ERROR_PIPE_BUSY` → treat it as *Busy*
  (one debugger session at a time).
* Created with `PIPE_REJECT_REMOTE_CLIENTS`: local machine only.
* The pipe name is **never fixed**: `bluesim.exe --debug-pipe <name>`.
  The name may be given with or without the `\\.\pipe\` prefix.
  The future AGDI DLL should generate a unique name per session, e.g.
  `\\.\pipe\BlueBridgeSimulator.Debug.<keil pid>.<nonce>`.
* Framing: a 24-byte header + `payloadSize` payload bytes, little-endian,
  hand-encoded. **Never** `memcpy` a host struct onto the wire.
* `PIPE_REJECT_REMOTE_CLIENTS`, but no custom DACL yet (documented limitation).

## 2. Header (24 bytes, little-endian)

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0  | u32 | `magic`     | `0x44474242` = bytes `'B' 'B' 'G' 'D'` |
| 4  | u16 | `versionMajor` | 1 |
| 6  | u16 | `versionMinor` | 0 |
| 8  | u16 | `packetKind` | 1 = Request, 2 = Response, 3 = Event |
| 10 | u16 | `opcode` | see §4 |
| 12 | u32 | `requestId` | chosen by the client, echoed by the server (0 in events) |
| 16 | u32 | `payloadSize` | ≤ `kMaxPayload` |
| 20 | u32 | `status` | 0 in requests; `WireStatus` in responses; 0 in events |

Golden bytes are pinned by `tests/debug_ipc_codec_test.cpp`.

## 3. Limits

| Limit | Value |
|---|---|
| `kMaxPayload` (any payload) | 1 MiB (1048576) |
| `kMaxMemoryTransfer` (READ/WRITE_MEMORY) | 64 KiB |
| `kMaxProgramTransfer` (PROGRAM_WRITE) | 64 KiB |
| register batch (`READ/WRITE_REGISTERS`) | 128 ids |
| response timeout (normal command) | 2000 ms |
| response timeout (PROGRAM_END) | 10000 ms |

A `payloadSize` above the limit is answered with `ProtocolError` **without
allocating** and the session is closed. A payload shorter than its declared
length, or with trailing garbage, is a `ProtocolError` too.

## 4. Opcodes

| Opcode | Name | Direction |
|---|---|---|
| 0x0001 | HELLO | request/response |
| 0x0002 | GET_CAPABILITIES | request/response |
| 0x0003 | GET_STATE | request/response |
| 0x0004 | GET_STOP_INFO | request/response |
| 0x0005 | HALT | request/response |
| 0x0006 | RESUME | request/response |
| 0x0007 | STEP | request/response |
| 0x0008 | RESET_HALT | request/response |
| 0x0009 | RESET_RUN | request/response |
| 0x000A | PING | request/response (answered by the pipe worker; never touches the target) |
| 0x0100 | READ_REGISTER | request/response |
| 0x0101 | WRITE_REGISTER | request/response |
| 0x0102 | READ_REGISTERS | request/response |
| 0x0103 | WRITE_REGISTERS | request/response |
| 0x0200 | READ_MEMORY | request/response |
| 0x0201 | WRITE_MEMORY | request/response |
| 0x0300 | ADD_BREAKPOINT | request/response |
| 0x0301 | REMOVE_BREAKPOINT | request/response |
| 0x0302 | CLEAR_BREAKPOINTS | request/response |
| 0x0400 | PROGRAM_BEGIN | request/response |
| 0x0401 | PROGRAM_ERASE | request/response |
| 0x0402 | PROGRAM_WRITE | request/response |
| 0x0403 | PROGRAM_END | request/response |
| 0x0404 | PROGRAM_ABORT | request/response |
| 0x8001 | TARGET_STOPPED | event (server → client) |
| 0x8002 | TARGET_FAULTED | event (server → client) |
| 0x8003 | PROGRAM_PROGRESS | reserved, not emitted in v1.0 |

## 5. Status codes (`WireStatus`)

| Value | Name | Meaning |
|---|---|---|
| 0 | Ok | success |
| 1 | InvalidCommand | unknown opcode / malformed arguments |
| 2 | InvalidState | e.g. STEP while running, PROGRAM_BEGIN while running |
| 3 | InvalidRegister | unknown register id (or the engine rejected it) |
| 4 | InvalidAddress | outside the guest memory map |
| 5 | Unsupported | valid but not implemented (e.g. **WRITE_MEMORY to flash**) |
| 6 | Busy | (pipe level) a debugger session already exists |
| 7 | Timeout | the owner loop did not answer in time; the session stays usable |
| 8 | ProtocolError | framing/version/handshake violation |
| 9 | TargetFault | the CPU could not continue |
| 10 | ProgramNotActive | no program transaction open |
| 11 | ProgramAlreadyActive | PROGRAM_BEGIN while a transaction is open |
| 12 | ProgramTokenInvalid | wrong / stale program token |
| 13 | ProgramRangeInvalid | erase/write outside the physical flash range |
| 14 | InternalError | engine error |

## 6. Payloads

All multi-byte fields are little-endian. Unlisted bytes are padding-free.

```
HELLO            req : u16 clientMajor, u16 clientMinor, u32 clientPid, u32 clientType
                 rsp : u16 serverMajor, u16 serverMinor, u32 serverPid, u32 sessionId,
                       u32 featureFlags, (u16 len + bytes) targetName,
                       (u16 len + bytes) boardName, (u16 len + bytes) architecture
GET_CAPABILITIES rsp : (u16 len + bytes) architecture, u32 endian (=1 little),
                       u32 capabilityFlags, u32 flashBase, u32 flashSize,
                       u32 flashAliasBase, u32 flashAliasSize, u32 sramBase, u32 sramSize,
                       u32 ccmBase, u32 ccmSize, u32 maxMemoryTransfer,
                       u32 maxProgramTransfer, u32 maxPayload
GET_STATE        rsp : u32 targetState, u32 stopReason, u32 pc, u64 virtualCycles,
                       u32 firmwareLoaded
GET_STOP_INFO    rsp : u32 stopReason, u32 pc
HALT/RESUME/STEP/RESET_HALT/RESET_RUN : (empty) -> (empty); status carries the result
PING             req : u64 cookie                    rsp : u64 cookie (echo)
READ_REGISTER    req : u32 registerId                rsp : u64 value
WRITE_REGISTER   req : u32 registerId, u64 value     rsp : (empty)
READ_REGISTERS   req : u32 count, u32 regId[count]
                 rsp : u32 count, { u32 regId, u64 value, u32 status }[count]
WRITE_REGISTERS  req : u32 count, { u32 regId, u64 value }[count]
                 rsp : u32 count, { u32 regId, u32 status }[count]
READ_MEMORY      req : u32 address, u32 length       rsp : raw bytes[length]
WRITE_MEMORY     req : u32 address, u32 length, bytes[length]   rsp : (empty)
ADD_BREAKPOINT   req : u32 address                   rsp : (empty)
REMOVE_BREAKPOINT req : u32 address                  rsp : (empty)
CLEAR_BREAKPOINTS req : (empty)                      rsp : (empty)
PROGRAM_BEGIN    req : u32 flags (optional, 0)        rsp : u64 token, u32 flashBase, u32 flashSize
PROGRAM_ERASE    req : u64 token, u32 address, u32 size         rsp : (empty)
PROGRAM_WRITE    req : u64 token, u32 address, u32 length, bytes[length]  rsp : (empty)
PROGRAM_END      req : u64 token                     rsp : u32 initialSP, u32 resetPC
PROGRAM_ABORT    req : u64 token                     rsp : (empty)
TARGET_STOPPED   evt : u32 stopReason, u32 pc, u64 virtualCycles
TARGET_FAULTED   evt : same layout, stopReason = 5 (Fault)
```

`length == 0` is an `Ok` no-op for READ_MEMORY / WRITE_MEMORY / PROGRAM_WRITE /
PROGRAM_ERASE (uniform across the protocol).

## 7. Target state / stop reason

```
targetState : 0 Halted, 1 Running, 2 Reset (transient; never observed), 3 Fault
stopReason  : 0 None, 1 UserHalt, 2 Breakpoint, 3 SingleStep, 4 Reset, 5 Fault
```

## 8. Register ids (frozen)

```
0..12  R0..R12        13 SP      14 LR      15 PC     16 XPSR
17 MSP   18 PSP   19 PRIMASK  20 BASEPRI  21 FAULTMASK  22 CONTROL
32..63 S0..S31        64 FPSCR
0xFFFFFFFF = invalid
```

## 9. Capability flags

```
bit0 exact single step      bit1 execution breakpoint   bit2 data watchpoint (never set)
bit3 FPU (S0-S31/FPSCR)     bit4 flash programming      bit5 async stop events
bit6 batch register read
```

The memory map in `GET_CAPABILITIES` is read from the **SoC model constants**
(flash 0x08000000/128 KiB, alias 0x00000000/128 KiB, SRAM 0x20000000/32 KiB,
CCM 0x10000000/16 KiB) — a client must not hard-code it.

**MMIO reads may have side effects**: a debug read of e.g. `USART_RDR` behaves
exactly like a CPU read (there is no side-effect-free peek in v1.0).

**Exact single step**: `exact single step = true` means `STEP` executes exactly
one guest instruction (unicorn `UC_CTL_EXACT_SINGLE_STEP`). Known limitation:
unicorn treats an ARM **IT block** (`IT` + controlled instruction) as ONE
execution unit, so a debugger has no instruction granularity inside an IT block.

## 10. Session / handshake

1. First request of a session **must** be `HELLO` (`PING` is also allowed).
   Anything else is answered with `ProtocolError` (no command executes).
2. A **major** version mismatch (header or HELLO payload) is answered with
   `ProtocolError` and the session is **closed**.
3. `HELLO` returns the server pid, a **new session id** (changes on every
   connect) and the target/board/architecture names.
4. `requestId` is client-chosen; the server echoes it. Responses and events may
   arrive interleaved — match responses by `requestId`, collect events.
5. A request that times out in the pipe worker is answered with `Timeout`; the
   command is abandoned and is never executed later (no stale responses). The
   request may be retried with a new `requestId`. The response budget is
   deadline-based (real clock, not a slice counter), so it is honoured within a
   few milliseconds of the nominal value (measured: 2012 ms for the 2 s budget).

## 11. Async events

The server pushes `TARGET_STOPPED` / `TARGET_FAULTED` whenever the **Simulator**
records a stop reason (UserHalt / Breakpoint / SingleStep / Reset / Fault) —
the event carries the Simulator's `StopInfo`, never a guess made by the IPC
layer. Events are only sent to a handshaken session and are dropped when a
session ends (a new session never sees the previous session's events).

Clients must therefore not rely on "I sent STEP, so the next packet is my
response": responses and events can interleave (the response of the command
that caused the stop is written **before** its event).

## 12. Disconnect semantics

When the pipe breaks (client exit, `ERROR_BROKEN_PIPE`, protocol violation):

1. the session stops accepting requests,
2. pending requests are cancelled/abandoned,
3. an active **program transaction is aborted** (live flash untouched),
4. the target is **halted**,
5. **debugger breakpoints are cleared**,
6. the session event queue is cleared.

The current firmware stays in the virtual flash and the target is **not**
reset. `bluesim.exe` keeps running (unless `--exit-on-debugger-disconnect`).
A reconnect starts a fresh session (new session id, new program token).
All of this runs on the simulator's owner thread via the command queue — the
pipe worker never touches the target.

## 13. Virtual flash programming

`WRITE_MEMORY` **never** writes flash: a flash address stays `Unsupported`
forever (a Keil memory-window edit must not reflash the board). Programming is
a separate transaction:

```
PROGRAM_BEGIN   -> token; staging = copy of the CURRENT virtual flash image
PROGRAM_ERASE   -> staging[range] = 0xFF            (staging only)
PROGRAM_WRITE   -> staging[range] = data            (staging only; out-of-order
                                                      and overlapping allowed,
                                                      the last write wins)
PROGRAM_END     -> atomic commit: live flash := staging, translated code
                   dropped, target reset, left HALTED with stopReason Reset
                   and SP/PC taken from the NEW vector table
PROGRAM_ABORT   -> staging dropped, live flash untouched
```

* Programming addresses are **physical** flash addresses (`0x08000000...`).
  The `0x00000000` alias is a read-only mirror: erase/write at the alias,
  outside the physical range, in RAM, or a range that overflows u32 are all
  `ProgramRangeInvalid`.
* `PROGRAM_BEGIN` requires a **halted** target (`InvalidState` otherwise, never
  an auto-halt); one transaction at a time (`ProgramAlreadyActive`).
* No 1→0 physical programming restriction: this is the debugger programming
  path, not a flash-controller model.
* Rejected erase/write keep the transaction active (the client decides to abort);
  an **internal** commit error auto-aborts.
* The program token is a monotonic 64-bit transaction counter salted with the
  session id — never 0, never a fixed 1.

## 14. Threading / implementation notes (for the AGDI author)

```
Named Pipe worker thread:  CreateNamedPipe / ConnectNamedPipe / ReadFile /
                           WriteFile / encode / decode / enqueue / wait /
                           forward events. NEVER touches Simulator, Unicorn,
                           STM32 or Board objects.
DebugCommandQueue:         thread-safe; control commands (HELLO/HALT/RESUME/
                           STEP/RESET_*) jump the queue; the owner pump is
                           budgeted (≤64 commands or ~2 ms per tick).
Simulator owner thread:    executes every command against IDebugTarget /
                           IFlashProgrammer and publishes async stop events.
```

The owner-thread pump must run even while the target is **Halted** (otherwise
STEP / READ_MEMORY / RESET after a halt would deadlock).