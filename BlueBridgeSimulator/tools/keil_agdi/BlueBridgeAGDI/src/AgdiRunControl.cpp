/*
 * Run control -- implementation (stage 7-2B checkpoint B.3).
 */

#include "AgdiRunControl.h"

#include <mutex>

#include "DebugIpcWire.h"

#include "AgdiBreakpoints.h"
#include "AgdiLog.h"
#include "AgdiSession.h"

namespace {

std::mutex g_mutex;              // protects the run bookkeeping ONLY; never held
                                 // while waiting for the target to stop
uint64_t g_generation = 0;
bool g_active = false;
uint32_t g_lastStopReason = bbipc::kWireStopNone;

const char *ReasonName(uint32_t reason) {
    switch (reason) {
        case bbipc::kWireStopNone:       return "None";
        case bbipc::kWireStopUserHalt:   return "UserHalt";
        case bbipc::kWireStopBreakpoint: return "Breakpoint";
        case bbipc::kWireStopSingleStep: return "SingleStep";
        case bbipc::kWireStopReset:      return "Reset";
        case bbipc::kWireStopFault:      return "Fault";
        default:                          return "?";
    }
}

/* Development/test cross-check (section 146): the stop reason is ALWAYS taken
 * from the event; GET_STOP_INFO only verifies it while Trace=1 is set. */
void CrossCheckStopInfo(uint32_t eventReason, uint32_t eventPc) {
    if (!AgdiLog::Trace()) return;
    uint32_t reason = 0, pc = 0;
    const bbx::RequestResult r = AgdiSession::Client().getStopInfo(&reason, &pc);
    if (!r.ok()) {
        AgdiLog::Write("Run", "stop-info cross-check failed status=%lu",
                       (unsigned long)r.status);
        return;
    }
    if (reason != eventReason || (pc & ~1u) != (eventPc & ~1u)) {
        AgdiLog::Write("Run", "STOP-INFO MISMATCH: event %s pc=0x%08lX vs "
                       "GET_STOP_INFO %s pc=0x%08lX",
                       ReasonName(eventReason), (unsigned long)eventPc,
                       ReasonName(reason), (unsigned long)pc);
    } else {
        AgdiLog::Write("Run", "stop-info cross-check ok (%s pc=0x%08lX)",
                       ReasonName(reason), (unsigned long)pc);
    }
}

void EndRun(uint64_t generation, uint32_t reason) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_generation == generation) {
        g_active = false;
        g_lastStopReason = reason;
    }
}

}  // namespace

namespace AgdiRunControl {

void ResetSession() {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_active = false;
    g_lastStopReason = bbipc::kWireStopNone;
    AgdiLog::Write("Run", "session reset (generation %llu kept monotonic)",
                   (unsigned long long)g_generation);
}

bool Active() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_active;
}

uint64_t Generation() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_generation;
}

Outcome Run(int timeoutMs, uint32_t tempAddress) {
    Outcome out;

    if (!AgdiSession::Usable()) {
        out.status = bbx::kStConnectionLost;
        out.lost = true;
        AgdiLog::Write("Run", "run refused: no usable session");
        return out;
    }

    uint64_t baseline = 0;
    bbx::IpcClient::StopBaseline stopBase;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        out.generation = ++g_generation;
        g_active = true;
    }
    /* Stop-state + event baseline recorded BEFORE RESUME: an event that arrives
     * between the write and the wait is already visible to the predicate, and a
     * LATE leftover event of the previous step/reset/halt (which repeats this
     * stop state) is filtered by waitForRunStop (sections 16-18). */
    stopBase = AgdiSession::Client().captureStopBaseline();
    baseline = stopBase.eventSequence;
    AgdiLog::Write("Run", "generation %llu: event baseline seq=%llu, pre-run stop "
                   "state %s pc=0x%08lX cycles=%llu (valid=%d)",
                   (unsigned long long)out.generation,
                   (unsigned long long)baseline, ReasonName(stopBase.reason),
                   (unsigned long)stopBase.pc, (unsigned long long)stopBase.cycles,
                   stopBase.valid ? 1 : 0);

    const bool useTemp = tempAddress != kNoTemporaryAddress;
    bool tempInstalled = false;
    if (useTemp) {
        const uint32_t st = AgdiBreakpoints::AddTemporary(tempAddress);
        if (st != bbipc::kStOk) {
            out.status = st;
            out.lost = (st == bbx::kStConnectionLost || st == bbx::kStNotConnected);
            EndRun(out.generation, bbipc::kWireStopNone);
            AgdiLog::Write("Run", "generation %llu: temporary breakpoint 0x%08lX "
                           "failed status=%lu", (unsigned long long)out.generation,
                           (unsigned long)tempAddress, (unsigned long)st);
            return out;
        }
        tempInstalled = true;
    }

    /* RESUME round trip: an ordinary (short) RPC. */
    const bbx::RequestResult r = AgdiSession::Client().resume();
    out.status = r.status;
    if (r.status != bbipc::kStOk) {
        out.lost = r.lost();
        if (tempInstalled) AgdiBreakpoints::RemoveTemporary(tempAddress);
        EndRun(out.generation, bbipc::kWireStopNone);
        AgdiLog::Write("Run", "generation %llu: RESUME failed status=%lu%s",
                       (unsigned long long)out.generation, (unsigned long)r.status,
                       r.lost() ? " (connection lost)" : "");
        return out;
    }
    out.accepted = true;

    /* Execution wait: sequence + stop-state based, no RPC timeout. The waiter
     * wakes on a new target event, on connection loss or on session teardown. */
    bbx::TargetEvent ev;
    const bbx::EventWaitResult wr =
        AgdiSession::Client().waitForRunStop(stopBase, timeoutMs, &ev);

    if (tempInstalled) {
        /* ALWAYS removed again -- target hit, user halt, fault, timeout or
         * connection loss (section 48). Removal is skipped automatically when
         * the session is gone (the simulator cleared its breakpoints itself). */
        AgdiBreakpoints::RemoveTemporary(tempAddress);
    }

    switch (wr) {
        case bbx::EventWaitResult::Event:
            out.stopped = true;
            out.stopReason = ev.stopReason;
            out.pc = ev.pc & ~1u;
            out.cycles = ev.cycles;
            EndRun(out.generation, ev.stopReason);
            CrossCheckStopInfo(ev.stopReason, ev.pc);
            AgdiLog::Write("Run", "generation %llu stopped: %s pc=0x%08lX "
                           "cycles=%llu (event seq %llu)",
                           (unsigned long long)out.generation,
                           ReasonName(ev.stopReason), (unsigned long)out.pc,
                           (unsigned long long)ev.cycles,
                           (unsigned long long)ev.sequence);
            break;

        case bbx::EventWaitResult::ConnectionLost:
        case bbx::EventWaitResult::Shutdown:
            out.lost = true;
            out.status = bbx::kStConnectionLost;
            EndRun(out.generation, bbipc::kWireStopNone);
            AgdiLog::Write("Run", "generation %llu: connection lost while the "
                           "target was running (run waiter woken)",
                           (unsigned long long)out.generation);
            break;

        case bbx::EventWaitResult::Timeout:
            out.timedOut = true;
            out.status = bbipc::kStTimeout;
            EndRun(out.generation, bbipc::kWireStopNone);
            AgdiLog::Write("Run", "generation %llu: execution wait timed out",
                           (unsigned long long)out.generation);
            break;
    }
    return out;
}

uint32_t Stop() {
    if (!AgdiSession::Usable()) return 1;   // nothing to stop, report "stopped"

    const bool wasActive = Active();
    const bbx::RequestResult r = AgdiSession::Client().halt();
    if (r.status == bbipc::kStOk) {
        AgdiLog::Write("Run", "AG_STOPRUN -> HALT ok (generation %llu, active=%d)",
                       (unsigned long long)Generation(), wasActive ? 1 : 0);
        return 1;
    }
    if (r.lost()) {
        AgdiLog::Write("Run", "AG_STOPRUN -> HALT lost the connection");
        return 1;
    }
    /* The target did not accept the halt (e.g. already halted): report the
     * official "still executing" answer so µVision keeps waiting. */
    AgdiLog::Write("Run", "AG_STOPRUN -> HALT status=%lu (not stopped)",
                   (unsigned long)r.status);
    return 0;
}

}  // namespace AgdiRunControl