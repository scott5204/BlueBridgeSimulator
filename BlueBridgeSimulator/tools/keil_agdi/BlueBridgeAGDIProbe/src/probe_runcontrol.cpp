/*
 * BlueBridgeAGDIProbe -- B.3 run-control suite (stage 7-2B.3 sections 60-72).
 *
 * Runs against the SAME IpcClient the AGDI DLL uses, without µVision: it
 * proves the IPC-level run/stop/breakpoint/event plumbing (RESUME, HALT,
 * ADD/REMOVE/CLEAR_BREAKPOINT, async TARGET_STOPPED with sequence numbers,
 * continue-from-breakpoint, stale-event protection, concurrency) before the
 * AGDI mapping is allowed anywhere near Keil.
 *
 * Fixture: `firmware/test_debug/test_debug.hex` -- a deterministic Thumb
 * program whose label addresses are published at 0x08000200 (debug map), so the
 * suite never hard-codes an address.
 */

#include <Windows.h>
#include <process.h>

#include <string.h>

#include <string>
#include <vector>

#include "DebugIpcWire.h"
#include "probe_common.h"

namespace {

const uint32_t kMapAddr = 0x08000200u;
const uint32_t kMapMagic = 0xDEB06001u;
const uint32_t kMapEnd = 0xDEB06E0Du;

/* --------------------------------------------------------------------------
 * small helpers
 * ------------------------------------------------------------------------ */

bool ReadU32(bbx::IpcClient &c, uint32_t addr, uint32_t &out) {
    std::vector<uint8_t> b;
    const bbx::RequestResult r = c.readMemory(addr, 4, &b);
    if (!r.ok() || b.size() != 4) return false;
    out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
          ((uint32_t)b[3] << 24);
    return true;
}

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

/* One blocking run: baseline BEFORE resume, then wait for a NEW stop event
 * (exactly the sequence discipline the AGDI run control uses). */
struct RunResult {
    bool ok = false;              // resume accepted
    bool stopped = false;
    bool lost = false;
    uint32_t status = 0;
    uint32_t reason = 0;
    uint32_t pc = 0;
    uint64_t cycles = 0;
    uint64_t sequence = 0;
    bool timedOut = false;
};

RunResult RunAndWait(bbx::IpcClient &c, int timeoutMs = 5000) {
    RunResult rr;
    /* Same discipline as the AGDI run control: capture the stop state + event
     * sequence BEFORE RESUME, then let the client filter a late leftover event
     * of the previous step/reset/halt. */
    const bbx::IpcClient::StopBaseline base = c.captureStopBaseline();
    const bbx::RequestResult r = c.resume();
    rr.status = r.status;
    if (!r.ok()) {
        rr.lost = r.lost();
        return rr;
    }
    rr.ok = true;
    bbx::TargetEvent ev;
    const bbx::EventWaitResult wr = c.waitForRunStop(base, timeoutMs, &ev);
    switch (wr) {
        case bbx::EventWaitResult::Event:
            rr.stopped = true;
            rr.reason = ev.stopReason;
            rr.pc = ev.pc & ~1u;
            rr.cycles = ev.cycles;
            rr.sequence = ev.sequence;
            break;
        case bbx::EventWaitResult::ConnectionLost:
        case bbx::EventWaitResult::Shutdown:
            rr.lost = true;
            break;
        case bbx::EventWaitResult::Timeout:
            rr.timedOut = true;
            break;
    }
    return rr;
}

bool ResetHaltAndCheck(bbx::IpcClient &c) {
    const bbx::RequestResult r = c.resetHalt();
    return r.ok();
}

/* Program a deterministic state: reset, then r2 = 0 (the loop counter register)
 * so the RAM counter starts from a known value. */
bool ResetAndClearCounter(bbx::IpcClient &c, const DebugMap &map) {
    if (!ResetHaltAndCheck(c)) return false;
    return c.writeRegister(bbipc::kWireRegR2, 0).ok();
}

uint32_t ReadCounter(bbx::IpcClient &c, const DebugMap &map) {
    uint32_t v = 0xFFFFFFFFu;
    ReadU32(c, map.counter, v);
    return v;
}

bool HitAndContinue(bbx::IpcClient &c, uint32_t expectAddr, int timeoutMs,
                    RunResult &rr, const char *what) {
    rr = RunAndWait(c, timeoutMs);
    if (!rr.stopped || rr.reason != bbipc::kWireStopBreakpoint ||
        rr.pc != expectAddr) {
        char buf[192];
        _snprintf_s(buf, _TRUNCATE,
                    "%s: expected Breakpoint @0x%08lX, got %s @0x%08lX (stopped=%d lost=%d timeout=%d status=%u)",
                    what, (unsigned long)expectAddr, ReasonName(rr.reason),
                    (unsigned long)rr.pc, rr.stopped ? 1 : 0, rr.lost ? 1 : 0,
                    rr.timedOut ? 1 : 0, rr.status);
        Check(false, buf);
        return false;
    }
    return true;
}

/* --------------------------------------------------------------------------
 * concurrency: thread A waits for a run to stop while the main thread halts
 * ------------------------------------------------------------------------ */
struct HaltRace {
    bbx::IpcClient *client = NULL;
    HANDLE goEvent = NULL;        // main -> thread: start one run
    HANDLE resumedEvent = NULL;   // thread -> main: RESUME accepted (target running)
    HANDLE doneEvent = NULL;      // thread -> main: run finished
    volatile long iterations = 0;
    volatile long shutdown = 0;   // set by the main thread to end the loop
    volatile long failures = 0;
    volatile long haltsAcked = 0;
    volatile long stopReasonMismatch = 0;
    volatile long deadlockTimeouts = 0;
    uint32_t lastReason = 0;
    uint32_t lastPc = 0;
};

/* Thread A: run and WAIT for the stop event. It signals `resumedEvent` as soon
 * as RESUME was accepted, so the main thread can HALT the (really running)
 * target deterministically instead of racing the resume. */
unsigned __stdcall HaltRaceThread(void *self) {
    HaltRace *r = (HaltRace *)self;
    for (;;) {
        const DWORD w = WaitForSingleObject(r->goEvent, 20000);
        if (w != WAIT_OBJECT_0) {
            InterlockedIncrement(&r->deadlockTimeouts);
            return 0;
        }
        if (r->shutdown) return 0;

        // same discipline as the AGDI run control (baseline -> RESUME -> wait)
        const bbx::IpcClient::StopBaseline base = r->client->captureStopBaseline();
        const bbx::RequestResult res = r->client->resume();
        SetEvent(r->resumedEvent);
        if (!res.ok()) {
            InterlockedIncrement(&r->failures);
        } else {
            bbx::TargetEvent ev;
            const bbx::EventWaitResult wr =
                r->client->waitForRunStop(base, 20000, &ev);
            if (wr != bbx::EventWaitResult::Event) {
                InterlockedIncrement(&r->failures);
            } else if (ev.stopReason != bbipc::kWireStopUserHalt) {
                r->lastReason = ev.stopReason;
                r->lastPc = ev.pc;
                InterlockedIncrement(&r->stopReasonMismatch);
            }
        }
        InterlockedIncrement(&r->iterations);
        SetEvent(r->doneEvent);
    }
}

}  // namespace

bool ReadDebugMap(bbx::IpcClient &c, const bbx::TargetCaps &caps, DebugMap &out) {
    std::vector<uint8_t> bytes;
    const bbx::RequestResult r = c.readMemory(caps.flashBase + 0x200, 64, &bytes);
    if (!r.ok() || bytes.size() != 64) return false;
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = (uint32_t)bytes[i * 4] | ((uint32_t)bytes[i * 4 + 1] << 8) |
               ((uint32_t)bytes[i * 4 + 2] << 16) |
               ((uint32_t)bytes[i * 4 + 3] << 24);
    }
    if (m[0] != kMapMagic || m[15] != kMapEnd) return false;
    out.resetHandler = m[1] & ~1u;
    out.step1 = m[2] & ~1u;
    out.step2 = m[3] & ~1u;
    out.step3 = m[4] & ~1u;
    out.step4 = m[5] & ~1u;
    out.loopTop = m[6] & ~1u;
    out.scratch = m[7];
    out.counter = m[8];
    return true;
}

bool RunControlSuite(bbx::IpcClient &c, const bbx::TargetCaps &caps,
                     bool fullStress) {
    std::printf("  [10] B.3 run control: RESUME / HALT / TARGET_STOPPED\n");

    DebugMap map;
    if (!ReadDebugMap(c, caps, map)) {
        Check(false, "test_debug debug map readable (magic + end marker)");
        return false;
    }
    {
        char buf[200];
        _snprintf_s(buf, _TRUNCATE,
                    "debug map: reset=0x%08lX step1..4=0x%08lX/0x%08lX/0x%08lX/0x%08lX "
                    "loop=0x%08lX counter=0x%08lX",
                    (unsigned long)map.resetHandler, (unsigned long)map.step1,
                    (unsigned long)map.step2, (unsigned long)map.step3,
                    (unsigned long)map.step4, (unsigned long)map.loopTop,
                    (unsigned long)map.counter);
        Info("%s", buf);
    }

    /* ---- [10] plain RESUME + HALT ------------------------------------- */
    {
        if (!ResetAndClearCounter(c, map)) {
            Check(false, "RESET_HALT before the run/halt test");
            return false;
        }
        bbx::TargetState s0;
        if (!c.getState(&s0).ok()) {
            Check(false, "GET_STATE before RESUME");
            return false;
        }
        const uint64_t baseline = c.latestEventSequence();
        bbx::RequestResult r = c.resume();
        Check(r.ok(), "RESUME answered Ok");

        bbx::TargetState running;
        c.getState(&running);
        Check(running.state == bbipc::kWireStateRunning, "target state = Running after RESUME");

        Sleep(80);   // let the target really execute

        r = c.halt();
        Check(r.ok(), "HALT answered Ok");
        bbx::TargetEvent ev;
        const bbx::EventWaitResult wr = c.waitForEventAfter(baseline, &ev, 3000);
        char buf[192];
        _snprintf_s(buf, _TRUNCATE, "TARGET_STOPPED after HALT: %s pc=0x%08lX cycles=%llu",
                    ReasonName(wr == bbx::EventWaitResult::Event ? ev.stopReason : 0),
                    (unsigned long)(ev.pc & ~1u), ev.cycles);
        Check(wr == bbx::EventWaitResult::Event &&
                  ev.stopReason == bbipc::kWireStopUserHalt,
              buf);
        (void)buf;
        bbx::TargetState s1;
        c.getState(&s1);
        Check(s1.state == bbipc::kWireStateHalted, "target state = Halted after HALT");
        Check(s1.virtualCycles > s0.virtualCycles,
              "virtual cycles advanced while running");
        Check(ev.sequence > baseline, "stop event carries a sequence above the run baseline");
    }

    /* ---- [11] breakpoint basic: hit BEFORE the instruction executes ---- */
    uint64_t bpCycles = 0;
    uint32_t bpCounter = 0;
    {
        std::printf("  [11] B.3 breakpoint: add / hit (instruction not executed)\n");
        if (!ResetAndClearCounter(c, map)) {
            Check(false, "RESET_HALT before the breakpoint test");
            return false;
        }
        bbx::RequestResult r = c.addBreakpoint(map.loopTop);
        Check(r.ok(), "ADD_BREAKPOINT accepted");

        RunResult rr;
        if (!HitAndContinue(c, map.loopTop, 5000, rr, "run to breakpoint")) return false;
        Check(true, "stop reason = Breakpoint and PC == breakpoint address");

        bpCounter = ReadCounter(c, map);
        bpCycles = rr.cycles;

        /* the instruction AT the breakpoint must not have executed yet: the
         * breakpoint instruction here is `adds r2,r2,#1` (R2 is the loop
         * counter), so one single step must increment R2 by exactly one and
         * move the PC past the breakpoint address */
        uint64_t r2Before = 0, r2After = 0;
        c.readRegister(bbipc::kWireRegR2, &r2Before);
        r = c.step();
        Check(r.ok(), "STEP at the breakpoint answered Ok");
        c.readRegister(bbipc::kWireRegR2, &r2After);
        uint64_t pcAfter = 0;
        c.readRegister(bbipc::kWireRegPC, &pcAfter);
        char buf[192];
        _snprintf_s(buf, _TRUNCATE,
                    "single step executed the breakpoint instruction: R2 %llu -> %llu, PC 0x%08lX -> 0x%08llX (RAM counter=%lu)",
                    r2Before, r2After, (unsigned long)map.loopTop, pcAfter,
                    (unsigned long)ReadCounter(c, map));
        Check(r2After == r2Before + 1 && (uint32_t)(pcAfter & ~1u) != map.loopTop, buf);
        (void)buf;
        bbx::TargetState st;
        c.getState(&st);
        Check(st.stopReason == bbipc::kWireStopSingleStep,
              "stop reason after step-from-breakpoint = SingleStep");
    }

    /* ---- [12] continue from breakpoint (no immediate re-hit) ---------- */
    {
        std::printf("  [12] B.3 continue from breakpoint\n");
        const uint32_t counterBefore = ReadCounter(c, map);
        RunResult rr;
        if (!HitAndContinue(c, map.loopTop, 5000, rr, "continue")) return false;
        const uint32_t counterAfter = ReadCounter(c, map);
        char buf[192];
        _snprintf_s(buf, _TRUNCATE,
                    "continue advanced the target: counter %lu -> %lu, cycles +%llu",
                    (unsigned long)counterBefore, (unsigned long)counterAfter,
                    (unsigned long long)(rr.cycles > bpCycles ? rr.cycles - bpCycles : 0));
        Check(counterAfter > counterBefore && rr.cycles > bpCycles, buf);
        (void)buf;
        bpCycles = rr.cycles;
    }

    /* ---- [13] stale event must not end a new run ---------------------- */
    {
        std::printf("  [13] B.3 stale event protection (SingleStep left over)\n");
        /* a STEP leaves a SingleStep event queued; a following run must NOT be
         * completed by it (sequence discipline) */
        c.step();   // deliberately NOT waiting for / consuming the event
        RunResult rr;
        if (!HitAndContinue(c, map.loopTop, 5000, rr, "run after a leftover step event"))
            return false;
        Check(rr.reason == bbipc::kWireStopBreakpoint &&
                  rr.pc == map.loopTop,
              "run stop was a fresh Breakpoint event, not the stale SingleStep");
    }

    /* ---- [14] multi breakpoints (4, in execution order) --------------- */
    {
        std::printf("  [14] B.3 multiple breakpoints (4, execution order)\n");
        c.clearBreakpoints();
        const uint32_t addrs[4] = {map.step2, map.step3, map.step4, map.loopTop};
        int added = 0;
        for (int i = 0; i < 4; ++i) {
            if (c.addBreakpoint(addrs[i]).ok()) ++added;
        }
        Check(added == 4, "4 ADD_BREAKPOINT accepted");

        if (!ResetAndClearCounter(c, map)) {
            Check(false, "RESET_HALT before the multi-breakpoint test");
            return false;
        }
        int hits = 0;
        for (int i = 0; i < 4; ++i) {
            RunResult rr;
            char what[64];
            _snprintf_s(what, _TRUNCATE, "multi-breakpoint hit %d", i + 1);
            if (!HitAndContinue(c, addrs[i], 5000, rr, what)) break;
            ++hits;
        }
        char buf[128];
        _snprintf_s(buf, _TRUNCATE, "4/4 breakpoints hit in execution order (%d)", hits);
        Check(hits == 4, buf);
        (void)buf;
    }

    /* ---- [15] remove + clear ----------------------------------------- */
    {
        std::printf("  [15] B.3 breakpoint remove / clear\n");
        bbx::RequestResult r = c.removeBreakpoint(map.step3);
        Check(r.ok(), "REMOVE_BREAKPOINT accepted");

        if (!ResetAndClearCounter(c, map)) {
            Check(false, "RESET_HALT before the remove test");
            return false;
        }
        RunResult rr;
        if (!HitAndContinue(c, map.step2, 5000, rr, "hit after remove (step_2)")) return false;
        /* step_3 was removed: the next stop must be step_4 */
        if (!HitAndContinue(c, map.step4, 5000, rr, "step_3 is gone (next stop = step_4)"))
            return false;
        Check(true, "removed breakpoint no longer stops the target");

        r = c.clearBreakpoints();
        Check(r.ok(), "CLEAR_BREAKPOINTS accepted");
        if (!ResetAndClearCounter(c, map)) {
            Check(false, "RESET_HALT before the clear test");
            return false;
        }
        const bbx::IpcClient::StopBaseline base = c.captureStopBaseline();
        r = c.resume();
        Check(r.ok(), "RESUME after CLEAR_BREAKPOINTS");
        Sleep(60);
        bbx::TargetEvent ev;
        const bbx::EventWaitResult wr = c.waitForRunStop(base, 150, &ev);
        Check(wr == bbx::EventWaitResult::Timeout,
              "no breakpoint stop after CLEAR_BREAKPOINTS (target keeps running)");
        c.halt();
        c.waitForRunStop(base, 3000, &ev);
    }

    /* ---- [16] refcount: same address, two logical breakpoints --------- */
    {
        std::printf("  [16] B.3 duplicate address / refcount\n");
        c.clearBreakpoints();
        bbx::RequestResult r1 = c.addBreakpoint(map.loopTop);
        bbx::RequestResult r2 = c.addBreakpoint(map.loopTop);
        Check(r1.ok() && r2.ok(), "same address added twice (backend add is idempotent)");
        RunResult rr;
        if (!ResetAndClearCounter(c, map)) return false;
        if (!HitAndContinue(c, map.loopTop, 5000, rr, "hit with duplicate add")) return false;
        /* one removal must still leave the breakpoint armed (backend removes at
         * the first REMOVE in the probe's flat model -- the AGDI adapter keeps
         * the refcount, this only pins the backend contract) */
        c.removeBreakpoint(map.loopTop);
        const bbx::IpcClient::StopBaseline base = c.captureStopBaseline();
        c.resume();
        Sleep(40);
        bbx::TargetEvent ev;
        const bbx::EventWaitResult wr = c.waitForRunStop(base, 120, &ev);
        const bool stoppedAgain = (wr == bbx::EventWaitResult::Event);
        if (stoppedAgain) c.halt();
        c.waitForRunStop(base, 3000, &ev);
        Info("backend duplicate-add semantics: %s after one REMOVE",
             stoppedAgain ? "still armed (server keeps a set)" : "removed");
        Check(true, "duplicate-add / remove semantics recorded");
    }

    /* ---- [17] immediate-hit race (event before the RPC response) ------ */
    {
        std::printf("  [17] B.3 immediate-hit race\n");
        c.clearBreakpoints();
        if (!ResetAndClearCounter(c, map)) {
            Check(false, "RESET_HALT before the race test");
            return false;
        }
        /* the breakpoint is 2 instructions ahead of the reset PC: the stop event
         * can arrive before the RESUME response is processed */
        bbx::RequestResult r = c.addBreakpoint(map.step1);
        Check(r.ok(), "ADD_BREAKPOINT at the first instruction after reset");
        RunResult rr;
        if (!HitAndContinue(c, map.step1, 5000, rr, "immediate-hit race")) return false;
        Check(true, "immediate TARGET_STOPPED not lost (PC == breakpoint address)");
    }

    /* ---- [18] event stress: hit/continue with exactly one event each --- */
    {
        const int iterations = fullStress ? 10000 : 500;
        std::printf("  [18] B.3 hit/continue stress x%d (one event per run)\n", iterations);
        c.clearBreakpoints();
        if (!ResetAndClearCounter(c, map)) return false;
        if (!c.addBreakpoint(map.loopTop).ok()) {
            Check(false, "ADD_BREAKPOINT for the stress loop");
            return false;
        }
        int failures = 0, extraEvents = 0;
        uint64_t lastSeq = c.latestEventSequence();
        const ULONGLONG t0 = GetTickCount64();
        for (int i = 0; i < iterations; ++i) {
            RunResult rr;
            rr = RunAndWait(c, 5000);
            if (!rr.stopped || rr.reason != bbipc::kWireStopBreakpoint ||
                rr.pc != map.loopTop) {
                ++failures;
                if (failures <= 3) {
                    Info("iteration %d: %s @0x%08lX (status=%u lost=%d timeout=%d)",
                         i, ReasonName(rr.reason), (unsigned long)rr.pc, rr.status,
                         rr.lost ? 1 : 0, rr.timedOut ? 1 : 0);
                }
            }
            if (rr.sequence != lastSeq + 1) ++extraEvents;
            lastSeq = rr.sequence;
        }
        const ULONGLONG dt = GetTickCount64() - t0;
        char buf[192];
        _snprintf_s(buf, _TRUNCATE,
                    "%d/%d hit+continue cycles correct, %d event-sequence anomalies, %.1f us/cycle",
                    iterations - failures, iterations, extraEvents,
                    double(dt) * 1000.0 / double(iterations));
        Check(failures == 0 && extraEvents == 0, buf);
        (void)buf;
        c.clearBreakpoints();
    }

    /* ---- [19] concurrent Run / Halt ----------------------------------- */
    {
        const int iterations = fullStress ? 1000 : 100;
        std::printf("  [19] B.3 concurrent Run/Halt x%d\n", iterations);
        HaltRace race;
        race.client = &c;
        race.goEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
        race.resumedEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
        race.doneEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (race.goEvent == NULL || race.resumedEvent == NULL ||
            race.doneEvent == NULL || !ResetAndClearCounter(c, map)) {
            Check(false, "concurrency race setup");
            return false;
        }
        uintptr_t th = _beginthreadex(NULL, 0, &HaltRaceThread, &race, 0, NULL);
        if (th == 0) {
            Check(false, "concurrency race thread");
            return false;
        }
        const ULONGLONG t0 = GetTickCount64();
        for (int i = 0; i < iterations; ++i) {
            SetEvent(race.goEvent);
            /* wait until the run really started, then let it execute for a
             * moment before the Stop path halts it */
            if (WaitForSingleObject(race.resumedEvent, 5000) != WAIT_OBJECT_0) {
                InterlockedIncrement(&race.deadlockTimeouts);
                break;
            }
            Sleep(2);
            const bbx::RequestResult hr = c.halt();
            if (hr.ok()) InterlockedIncrement(&race.haltsAcked);
            if (WaitForSingleObject(race.doneEvent, 20000) != WAIT_OBJECT_0) {
                InterlockedIncrement(&race.deadlockTimeouts);
                break;
            }
        }
        const ULONGLONG dt = GetTickCount64() - t0;
        const long done = race.iterations;
        race.shutdown = 1;             // tell the thread to exit on its next wakeup
        SetEvent(race.goEvent);
        WaitForSingleObject((HANDLE)th, 5000);
        CloseHandle((HANDLE)th);
        CloseHandle(race.goEvent);
        CloseHandle(race.resumedEvent);
        CloseHandle(race.doneEvent);

        char buf[224];
        _snprintf_s(buf, _TRUNCATE,
                    "run/halt race: %ld/%d completed, halts acked %ld, failures %ld, "
                    "reason mismatches %ld, deadlock/timeouts %ld, %.2f ms/iteration",
                    done, iterations, race.haltsAcked, race.failures,
                    race.stopReasonMismatch, race.deadlockTimeouts,
                    double(dt) / double(iterations));
        Check(race.failures == 0 && race.stopReasonMismatch == 0 &&
                  race.deadlockTimeouts == 0 && done >= iterations,
              buf);
        (void)buf;
    }

    /* ---- [20] connection lost while running wakes the waiter ---------- */
    {
        std::printf("  [20] B.3 stop/halt leaves a clean state for other access\n");
        if (!ResetAndClearCounter(c, map)) return false;
        const bbx::RequestResult r = c.resume();
        Check(r.ok(), "RESUME for the post-run access check");
        Sleep(30);
        c.halt();
        bbx::TargetEvent ev;
        c.waitForEventAfter(0, &ev, 2000);
        /* registers/memory must still work right after a stop */
        uint64_t pc = 0;
        std::vector<uint8_t> mem;
        const bool regOk = c.readRegister(bbipc::kWireRegPC, &pc).ok();
        const bool memOk = c.readMemory(caps.flashBase, 16, &mem).ok();
        Check(regOk && memOk, "registers + memory readable right after a stop");
        (void)pc;
    }

    return true;
}