#pragma once
/*
 * Run control (stage 7-2B checkpoint B.3, spec sections 19-27 / 45-54 / 136-138).
 *
 * Official AGDI semantics this implements (AppNote 173 / official AGDI.H):
 *
 *   AG_GoStep(AG_GOFORBRK)  -- run until a breakpoint / user halt / fault,
 *                              BLOCKING: the call returns when execution stops
 *   AG_GoStep(AG_GOTILADR)  -- same, but with an internal TEMPORARY breakpoint
 *                              at pA->Adr (always removed afterwards)
 *   AG_GoStep(AG_NSTEP)     -- n instruction steps (handled by the driver)
 *   AG_GoStep(AG_STOPRUN)   -- called from ANOTHER µVision thread while the Go
 *                              call is blocked; asks the target to stop
 *
 * IPC mapping: RESUME + wait for TARGET_STOPPED/TARGET_FAULTED (never a
 * GET_STATE poll loop, section 22). The wait is SEQUENCE based, so a stale
 * SingleStep/Reset event can never terminate a new run (sections 15-20), and it
 * is an EXECUTION wait: it has no 2 s RPC timeout (section 137) -- it ends only
 * on a stop event, connection loss, session teardown or an explicit internal
 * timeout.
 *
 * Locking (sections 9/10): the run state is protected by a tiny dedicated mutex
 * that is NEVER held while waiting for the target to stop; the wait itself is a
 * condition variable inside IpcClient (eventCv_). AG_GoStep(AG_STOPRUN) from
 * the Stop thread therefore reaches the HALT write immediately, even while the
 * Go thread is blocked.
 */

#include <stdint.h>

namespace AgdiRunControl {

enum : uint32_t {
    /* AG_GoStep(GOTILADR) value meaning "no temporary breakpoint". */
    kNoTemporaryAddress = 0xFFFFFFFFu,
};

struct Outcome {
    bool     accepted = false;      // RESUME round trip succeeded
    bool     stopped = false;       // a NEW stop event ended the wait
    bool     lost = false;          // connection / session failure
    bool     timedOut = false;      // only with a finite wait timeout
    uint32_t stopReason = 0;        // bbipc::WireStopReason when stopped
    uint32_t pc = 0;                // event PC when stopped
    uint64_t cycles = 0;            // virtual cycles when stopped
    uint64_t generation = 0;        // run generation (section 20)
    uint32_t status = 0;            // bbx/bbipc status of the failing step
};

/* Drops the run state (connect/disconnect). Never touches a live run. */
void ResetSession();

/* Runs until the target stops. `tempAddress` = kNoTemporaryAddress for a plain
 * continue; `timeoutMs` <= 0 waits until the target stops. Blocking: call from
 * the µVision Go thread only. */
Outcome Run(int timeoutMs, uint32_t tempAddress);

/* µVision Stop button (AG_GoStep(AG_STOPRUN), another thread). Sends HALT and
 * returns the official StopExec encoding: 1 = stopped, 0 = still executing. */
uint32_t Stop();

bool Active();
uint64_t Generation();

}  // namespace AgdiRunControl