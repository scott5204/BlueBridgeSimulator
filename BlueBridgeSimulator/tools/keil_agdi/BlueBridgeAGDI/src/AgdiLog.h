#pragma once
/*
 * BlueBridgeAGDI logging (spec 7-2B sections 19 / 128).
 *
 * Target file:  %LOCALAPPDATA%\BlueBridgeSimulator\logs\BlueBridgeAGDI.log
 * Line format:  <local time>  pid=.. tid=..  <function>  <details>
 *
 * Rules that matter:
 *  - never do file I/O inside DllMain (loader lock).  DllMain only records a
 *    pending line in memory; the first real log call flushes it.
 *  - µVision polls memory/registers thousands of times per second, so those
 *    calls go through WriteRateLimited() which keeps a per-second budget and
 *    emits "N calls suppressed" summaries instead of flooding the file.
 *  - a log file larger than kMaxLogBytes is rotated once (BlueBridgeAGDI.log.1).
 *
 * Everything in this module is internal to the DLL; no STL types cross the
 * ABI boundary.
 */

#include <Windows.h>

namespace AgdiLog {

const unsigned kMaxLogBytes = 8u * 1024u * 1024u;

/* In-memory only (safe under the loader lock). */
void Pending(const char *fmt, ...);

/* Normal log line. Opens the file lazily on first use. */
void Write(const char *fn, const char *fmt, ...);

/* IPC transport line: "<time> pid=.. tid=.. [IPC] <details>" (spec section 81).
 * Feed it the body only -- the tag is added here. */
void WriteIpc(const char *fmt, ...);

/* Rate-limited log line for hot paths (memory / register polling). */
void WriteRateLimited(const char *fn, const char *fmt, ...);

/* Trace flag (agdi.ini `Trace=`): when off, hot-path calls are not logged at
 * all; when on, they are logged through the rate limiter. */
void SetTrace(bool on);
bool Trace();

/* Verbatim application-load trace (agdi.ini `LoadTrace=`). When on, every
 * AG_MemAcc / AG_MemAtt / AG_GoStep / AG_AllReg / AG_RegAcc call is written as
 * `[TRACE]` with nCode/address/size and return value, WITHOUT the rate limiter
 * (no call may be suppressed). Off by default: this is a diagnostic for the
 * B.4 application-load work, not a normal operating mode. The µVision thread id
 * is already part of every line prefix (`tid=`), so the call sequence can be
 * reconstructed thread by thread. */
void SetVerbose(bool on);
bool Verbose();
void WriteVerbose(const char *fn, const char *fmt, ...);

/* Flush + close (called from DLL_PROCESS_DETACH). */
void Shutdown();

}  // namespace AgdiLog