/*
 * Keil Application Load -> virtual flash programming -- implementation
 * (stage 7-2B.4, checkpoint B.4.2).
 *
 * Threading: µVision serializes the load lifecycle in practice, but STARTLOAD /
 * WROPC / ENDLOAD / AG_UNINIT can arrive on different threads, so every entry
 * point takes the module mutex. It is held across the short PROGRAM_* RPCs on
 * purpose: the operations are logically sequential and the RPCs have their own
 * timeouts (PROGRAM_END uses the 10 s protocol timeout).
 */

#include "AgdiProgram.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include <string.h>

#include "AgdiLog.h"
#include "AgdiRunControl.h"
#include "AgdiSession.h"

namespace {

/* --------------------------------------------------------------------------
 * interval helpers (uint64 math only -- address + size can overflow uint32,
 * spec section 19)
 * ------------------------------------------------------------------------ */
struct Range {
    uint64_t b;
    uint64_t e;
};

void AddRange(std::vector<Range> &v, uint64_t b, uint64_t e) {
    if (e <= b) return;
    v.push_back(Range{b, e});
    std::sort(v.begin(), v.end(),
              [](const Range &x, const Range &y) { return x.b < y.b; });
    std::vector<Range> merged;
    for (const Range &r : v) {
        if (!merged.empty() && r.b <= merged.back().e) {
            if (r.e > merged.back().e) merged.back().e = r.e;
        } else {
            merged.push_back(r);
        }
    }
    v.swap(merged);
}

/* sorted `from` minus sorted, merged `minus` */
std::vector<Range> Subtract(const std::vector<Range> &from,
                            const std::vector<Range> &minus) {
    std::vector<Range> out;
    for (const Range &r : from) {
        uint64_t pos = r.b;
        for (const Range &m : minus) {
            if (m.e <= pos) continue;
            if (m.b >= r.e) break;
            if (m.b > pos) out.push_back(Range{pos, m.b});
            pos = m.e > pos ? m.e : pos;
            if (pos >= r.e) break;
        }
        if (pos < r.e) out.push_back(Range{pos, r.e});
    }
    return out;
}

/* --------------------------------------------------------------------------
 * address regions from GET_CAPABILITIES (never hard-coded, spec section 15)
 * ------------------------------------------------------------------------ */
enum class Region { Other, Flash, Ram, Alias };

Region Classify(uint64_t a, const bbx::TargetCaps &c) {
    if (c.flashSize > 0 && a >= c.flashBase &&
        a < uint64_t(c.flashBase) + c.flashSize) {
        return Region::Flash;
    }
    if (c.sramSize > 0 && a >= c.sramBase &&
        a < uint64_t(c.sramBase) + c.sramSize) {
        return Region::Ram;
    }
    if (c.ccmSize > 0 && a >= c.ccmBase &&
        a < uint64_t(c.ccmBase) + c.ccmSize) {
        return Region::Ram;
    }
    if (c.flashAliasSize > 0 && a >= c.flashAliasBase &&
        a < uint64_t(c.flashAliasBase) + c.flashAliasSize) {
        return Region::Alias;
    }
    return Region::Other;
}

uint64_t RegionEnd(uint64_t a, Region k, const bbx::TargetCaps &c) {
    switch (k) {
        case Region::Flash: return uint64_t(c.flashBase) + c.flashSize;
        case Region::Ram:
            if (c.sramSize > 0 && a >= c.sramBase &&
                a < uint64_t(c.sramBase) + c.sramSize) {
                return uint64_t(c.sramBase) + c.sramSize;
            }
            return uint64_t(c.ccmBase) + c.ccmSize;
        case Region::Alias: return uint64_t(c.flashAliasBase) + c.flashAliasSize;
        default:            return a;   // never walked
    }
}

uint32_t Le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* --------------------------------------------------------------------------
 * load session state (AgdiProgram.h / spec section 6)
 * ------------------------------------------------------------------------ */
enum class State { Idle, Active, Failed };

struct LoadSession {
    State        state = State::Idle;
    uint64_t     token = 0;
    bool         noCode = false;
    bool         incremental = false;
    std::string  appPath;          // informational; never parsed
    uint64_t     flashBytes = 0;
    uint64_t     ramBytes = 0;
    uint32_t     blockCount = 0;   // accepted AG_WROPC blocks
    uint32_t     eraseOps = 0;
    uint64_t     startTick = 0;
    std::vector<Range> mappedFlash;   // AG_MEMMAP flash declarations
    std::vector<Range> erased;        // staging ranges already erased
};

std::mutex  g_mutex;
LoadSession g_ses;

/* official AGDI error for an IPC status (programming-path failures) */
U32 AgdiErrorFor(uint32_t status) {
    if (status == bbx::kStNotConnected || status == bbx::kStConnectionLost ||
        status == bbx::kStWriteTimeout) {
        return AG_NOACCESS;
    }
    return AG_WRFAILED;
}

void ClearLocked(const char *why) {
    if (g_ses.state != State::Idle || g_ses.token != 0 || g_ses.blockCount != 0) {
        AgdiLog::Write("PROGRAM", "[PROGRAM] state cleared (%s)", why);
    }
    g_ses = LoadSession();
}

/* Failure path (spec sections 33/34): abort the open transaction immediately,
 * remember the failure (later WROPC blocks are refused, never a new begin) and
 * keep the live flash untouched (staging is discarded by PROGRAM_ABORT). */
U32 FailLocked(U32 agdiErr, const char *why) {
    if (g_ses.token != 0 && AgdiSession::Usable()) {
        const bbx::RequestResult r =
            AgdiSession::Client().programAbort(g_ses.token);
        AgdiLog::Write("PROGRAM", "[PROGRAM] abort reason=%s status=%lu "
                       "(staging discarded, live flash unchanged)",
                       why, (unsigned long)r.status);
    } else {
        AgdiLog::Write("PROGRAM", "[PROGRAM] abort not sent (%s; %s)", why,
                       g_ses.token == 0 ? "no token"
                                        : "session gone -- the server aborts on "
                                          "disconnect");
    }
    g_ses.token = 0;
    g_ses.state = State::Failed;
    AgdiLog::Write("PROGRAM", "[PROGRAM] load session FAILED (%s) -> AGDI "
                   "error %lu", why, (unsigned long)agdiErr);
    return agdiErr;
}

/* Make sure the target is halted before PROGRAM_BEGIN (spec sections 7/11/12):
 * an active Go/Step wait is ended through the official stop path, then HALT is
 * sent and GET_STATE confirms the halt. */
bool EnsureHaltedLocked(uint32_t *statusOut) {
    if (AgdiRunControl::Active()) {
        AgdiLog::Write("PROGRAM", "[PROGRAM] ending the active RunContext through "
                       "the official stop path before programming");
        AgdiRunControl::Stop();
    }
    for (int attempt = 0; attempt < 40; ++attempt) {
        bbx::TargetState st;
        const bbx::RequestResult r = AgdiSession::Client().getState(&st);
        if (r.lost()) {
            *statusOut = r.status;
            return false;
        }
        if (r.ok() && st.state != bbipc::kWireStateRunning) return true;
        if (attempt == 0 && r.ok() && st.state == bbipc::kWireStateRunning) {
            const bbx::RequestResult h = AgdiSession::Client().halt();
            if (h.lost()) {
                *statusOut = h.status;
                return false;
            }
        }
        ::Sleep(25);
    }
    *statusOut = bbipc::kStTimeout;
    return false;
}

/* erase ranges not erased yet -- every range is erased exactly once per
 * transaction (spec sections 20-23) */
U32 EraseRangesLocked(const std::vector<Range> &plan) {
    for (const Range &seg : plan) {
        const std::vector<Range> todo = Subtract(std::vector<Range>(1, seg),
                                                 g_ses.erased);
        for (const Range &part : todo) {
            uint64_t p = part.b;
            while (p < part.e) {
                const uint32_t chunk = (uint32_t)std::min<uint64_t>(
                    part.e - p, bbipc::kMaxProgramTransfer);
                const bbx::RequestResult r =
                    AgdiSession::Client().programErase(g_ses.token, (uint32_t)p,
                                                       chunk);
                if (!r.ok()) {
                    AgdiLog::Write("PROGRAM", "[PROGRAM] PROGRAM_ERASE 0x%08lX "
                                   "+%lu failed status=%lu", (unsigned long)p,
                                   (unsigned long)chunk, (unsigned long)r.status);
                    return FailLocked(AgdiErrorFor(r.status), "erase");
                }
                ++g_ses.eraseOps;
                AgdiLog::Write("PROGRAM", "[PROGRAM] erase 0x%08lX +%lu (staging)",
                               (unsigned long)p, (unsigned long)chunk);
                p += chunk;
            }
            AddRange(g_ses.erased, part.b, part.e);
        }
    }
    return AG_OK;
}

U32 WriteFlashLocked(uint32_t addr, const UC8 *data, uint32_t len) {
    /* erase plan = everything µVision declared via AG_MEMMAP (the image
     * footprint) plus this block: a smaller image therefore never leaves stale
     * bytes of a previous, larger one inside the programmed region, and no
     * range is erased twice (spec sections 21-27) */
    std::vector<Range> plan = g_ses.mappedFlash;
    AddRange(plan, addr, uint64_t(addr) + len);
    U32 nE = EraseRangesLocked(plan);
    if (nE != AG_OK) return nE;

    uint32_t done = 0;
    while (done < len) {
        const uint32_t chunk = std::min<uint32_t>(
            len - done, (uint32_t)bbipc::kMaxProgramTransfer);
        const bbx::RequestResult r = AgdiSession::Client().programWrite(
            g_ses.token, addr + done, data + done, chunk);
        if (!r.ok()) {
            AgdiLog::Write("PROGRAM", "[PROGRAM] PROGRAM_WRITE 0x%08lX +%lu "
                           "failed status=%lu", (unsigned long)(addr + done),
                           (unsigned long)chunk, (unsigned long)r.status);
            return FailLocked(AgdiErrorFor(r.status), "flash write");
        }
        done += chunk;
    }
    g_ses.flashBytes += len;
    return AG_OK;
}

U32 WriteRamLocked(uint32_t addr, const UC8 *data, uint32_t len) {
    uint32_t done = 0;
    while (done < len) {
        const uint32_t chunk = std::min<uint32_t>(
            len - done, (uint32_t)bbipc::kMaxMemoryTransfer);
        const bbx::RequestResult r = AgdiSession::Client().writeMemory(
            addr + done, data + done, chunk);
        if (!r.ok()) {
            AgdiLog::Write("PROGRAM", "[PROGRAM] RAM WRITE_MEMORY 0x%08lX +%lu "
                           "failed status=%lu", (unsigned long)(addr + done),
                           (unsigned long)chunk, (unsigned long)r.status);
            return FailLocked(AgdiErrorFor(r.status), "ram write");
        }
        done += chunk;
    }
    g_ses.ramBytes += len;
    AgdiLog::Write("PROGRAM", "[PROGRAM] ram write 0x%08lX +%lu (WRITE_MEMORY, "
                   "never staged)", (unsigned long)addr, (unsigned long)len);

    /* Development-stage readback (spec section 37 spirit): confirm the bytes
     * are visible NOW. The simulator deliberately zeroes SRAM/CCM on every
     * reset, and a load is always followed by a reset, so a RAM load region
     * cannot be observed after the session -- but it must be observable here. */
    std::vector<uint8_t> rb;
    if (AgdiSession::Client().readMemory(addr, len, &rb).ok() &&
        rb.size() == len) {
        const bool same = memcmp(rb.data(), data, len) == 0;
        AgdiLog::Write("PROGRAM", "[PROGRAM] ram readback 0x%08lX +%lu %s",
                       (unsigned long)addr, (unsigned long)len,
                       same ? "== written bytes (confirmed)"
                            : "MISMATCH (write lost?)");
    }
    return AG_OK;
}

/* Development-stage readback (spec section 37): the target must be halted and
 * the flash vector table must be the one PROGRAM_END reported. */
void VerifyVectorLocked(uint32_t sp, uint32_t pc) {
    const bbx::TargetCaps &caps = AgdiSession::Caps();
    bbx::TargetState st;
    if (AgdiSession::Client().getState(&st).ok() &&
        st.state == bbipc::kWireStateRunning) {
        AgdiLog::Write("PROGRAM", "[PROGRAM] WARNING: target running after "
                       "PROGRAM_END (expected Halted)");
    }
    std::vector<uint8_t> v;
    if (!AgdiSession::Client().readMemory(caps.flashBase, 8, &v).ok() ||
        v.size() != 8) {
        AgdiLog::Write("PROGRAM", "[PROGRAM] post-commit vector readback failed");
        return;
    }
    const uint32_t rsp = Le32(&v[0]);
    const uint32_t rpc = Le32(&v[4]);
    const bool ramOk = (rsp >= caps.sramBase &&
                        rsp < uint64_t(caps.sramBase) + caps.sramSize) ||
                       (rsp >= caps.ccmBase &&
                        rsp < uint64_t(caps.ccmBase) + caps.ccmSize);
    const bool flashOk = (rpc & ~1u) >= caps.flashBase &&
                         (rpc & ~1u) < uint64_t(caps.flashBase) + caps.flashSize;
    const bool match = (rsp == sp) && ((rpc & ~1u) == (pc & ~1u));
    AgdiLog::Write("PROGRAM", "[PROGRAM] verify flash vector SP=0x%08lX "
                   "PC=0x%08lX (PROGRAM_END reported sp=0x%08lX pc=0x%08lX, "
                   "match=%d, spInRam=%d, pcInFlash=%d)",
                   (unsigned long)rsp, (unsigned long)(rpc & ~1u),
                   (unsigned long)sp, (unsigned long)pc, match ? 1 : 0,
                   ramOk ? 1 : 0, flashOk ? 1 : 0);
}

}  // namespace

namespace AgdiProgram {

bool Active() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_ses.state == State::Active;
}

bool Failed() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_ses.state == State::Failed;
}

U32 BeginLoad(const LOADPARMS *parms) {
    std::lock_guard<std::mutex> lk(g_mutex);

    /* a new STARTLOAD always starts from Idle (spec section 72) */
    if (g_ses.state != State::Idle) ClearLocked("superseded by a new STARTLOAD");

    g_ses.noCode = (parms != NULL) && (parms->NoCode != 0);
    g_ses.incremental = (parms != NULL) && (parms->Incremental != 0);
    if (parms != NULL) {
        g_ses.appPath.assign(parms->szFile,
                             strnlen(parms->szFile, sizeof(parms->szFile)));
    }

    if (parms == NULL || parms->NoCode != 0) {
        /* spec section 8: NOCODE loads symbols only -- never PROGRAM_* */
        AgdiLog::Write("PROGRAM", "[PROGRAM] startload file=\"%s\" incremental=%u "
                       "noCode=%u -> symbols only, no flash transaction",
                       g_ses.appPath.c_str(), g_ses.incremental ? 1 : 0,
                       g_ses.noCode ? 1 : 0);
        return AG_OK;
    }

    AgdiLog::Write("PROGRAM", "[PROGRAM] startload file=\"%s\" incremental=%u "
                   "noCode=0", g_ses.appPath.c_str(), g_ses.incremental ? 1 : 0);

    if (!AgdiSession::Usable()) {
        g_ses.state = State::Failed;
        AgdiLog::Write("PROGRAM", "[PROGRAM] begin refused: no usable IPC "
                       "session -> AG_NOACCESS");
        return AG_NOACCESS;
    }

    uint32_t st = bbipc::kStOk;
    if (!EnsureHaltedLocked(&st)) {
        return FailLocked(st == bbipc::kStTimeout ? AG_INVALOP : AgdiErrorFor(st),
                          "target did not halt");
    }

    uint64_t token = 0;
    uint32_t flashBase = 0, flashSize = 0;
    const bbx::RequestResult r =
        AgdiSession::Client().programBegin(&token, &flashBase, &flashSize);
    if (!r.ok()) {
        AgdiLog::Write("PROGRAM", "[PROGRAM] PROGRAM_BEGIN failed status=%lu",
                       (unsigned long)r.status);
        return FailLocked(AgdiErrorFor(r.status), "program begin");
    }
    g_ses.token = token;
    g_ses.state = State::Active;
    g_ses.startTick = GetTickCount64();
    AgdiLog::Write("PROGRAM", "[PROGRAM] begin token=0x%016llX flash=0x%08lX/%luB "
                   "(staging = copy of the live flash)",
                   (unsigned long long)token, (unsigned long)flashBase,
                   (unsigned long)flashSize);
    return AG_OK;
}

U32 EndLoad() {
    std::lock_guard<std::mutex> lk(g_mutex);

    if (g_ses.state == State::Idle) {
        AgdiLog::Write("PROGRAM", "[PROGRAM] endload: no transaction (%s)",
                       g_ses.noCode ? "noCode=1 symbols-only load"
                                    : "idle, nothing was programmed");
        return AG_OK;   // spec section 35: NOCODE never reaches PROGRAM_END
    }

    if (g_ses.state == State::Failed) {
        AgdiLog::Write("PROGRAM", "[PROGRAM] endload after a FAILED session -> "
                       "no commit (live flash unchanged)");
        ClearLocked("failed session closed by ENDLOAD");
        return AG_WRFAILED;
    }

    uint32_t sp = 0, pc = 0;
    const bbx::RequestResult r =
        AgdiSession::Client().programEnd(g_ses.token, &sp, &pc);
    if (!r.ok()) {
        AgdiLog::Write("PROGRAM", "[PROGRAM] PROGRAM_END failed status=%lu "
                       "(no commit)", (unsigned long)r.status);
        if (r.lost()) {
            g_ses.token = 0;
            g_ses.state = State::Failed;
            return AG_NOACCESS;
        }
        return FailLocked(AG_WRFAILED, "program end");
    }

    const uint64_t ms = GetTickCount64() - g_ses.startTick;
    const unsigned blocks = g_ses.blockCount;
    const unsigned eraseOps = g_ses.eraseOps;
    const unsigned long long flashBytes = g_ses.flashBytes;
    const unsigned long long ramBytes = g_ses.ramBytes;
    AgdiLog::Write("PROGRAM", "[PROGRAM] end success: blocks=%u flash=%lluB "
                   "ram=%lluB eraseOps=%u duration=%llums "
                   "(atomic commit -> TCG invalidate -> reset+halt; µVision's "
                   "own AG_RESET follows)", blocks, flashBytes, ramBytes, eraseOps,
                   (unsigned long long)ms);
    VerifyVectorLocked(sp, pc);
    ClearLocked("committed");
    return AG_OK;
}

U32 WriteOpcodes(UL32 addr, const UC8 *data, UL32 many) {
    std::lock_guard<std::mutex> lk(g_mutex);

    if (g_ses.state != State::Active) {
        /* defensive: AgdiMemory only routes here while Active */
        AgdiLog::Write("PROGRAM", "[PROGRAM] AG_WROPC 0x%08lX +%lu refused: no "
                       "active load session", (unsigned long)addr,
                       (unsigned long)many);
        return AG_WRFAILED;
    }
    if (data == NULL || many == 0) return AG_OK;

    const bbx::TargetCaps &caps = AgdiSession::Caps();
    const uint64_t start = addr;
    const uint64_t end = start + uint64_t(many);

    uint64_t pos = start;
    while (pos < end) {
        const Region kind = Classify(pos, caps);
        if (kind == Region::Other) {
            AgdiLog::Write("PROGRAM", "[PROGRAM] AG_WROPC 0x%08lX +%llu refused: "
                           "not a programmable region (unknown ranges are never "
                           "guessed or remapped, spec section 17)",
                           (unsigned long)pos, (unsigned long long)(end - pos));
            return FailLocked(AG_WRFAILED, "non-programmable range");
        }
        const uint64_t stop = RegionEnd(pos, kind, caps);
        const uint64_t slice = (stop < end ? stop : end) - pos;
        const UC8 *src = data + size_t(pos - start);
        U32 nE = AG_OK;
        if (kind == Region::Flash) {
            nE = WriteFlashLocked((uint32_t)pos, src, (uint32_t)slice);
        } else if (kind == Region::Ram) {
            nE = WriteRamLocked((uint32_t)pos, src, (uint32_t)slice);
        } else {
            AgdiLog::Write("PROGRAM", "[PROGRAM] REFUSED flash alias 0x%08lX "
                           "+%llu inside a load block: aliases are never "
                           "programmed and never silently offset by "
                           "+0x08000000 (spec section 17)",
                           (unsigned long)pos, (unsigned long long)slice);
            return FailLocked(AG_WRFAILED, "flash alias in load block");
        }
        if (nE != AG_OK) return nE;
        pos += slice;
    }

    ++g_ses.blockCount;
    AgdiLog::Write("PROGRAM", "[PROGRAM] wropc block %u: 0x%08lX +%lu ok "
                   "(flash=%lluB ram=%lluB so far)", g_ses.blockCount,
                   (unsigned long)addr, (unsigned long)many,
                   (unsigned long long)g_ses.flashBytes,
                   (unsigned long long)g_ses.ramBytes);
    return AG_OK;
}

void NoteMappedRange(UL32 addr, UL32 len) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_ses.state != State::Active || len == 0) return;

    const bbx::TargetCaps &caps = AgdiSession::Caps();
    const uint64_t flashLo = caps.flashBase;
    const uint64_t flashHi = uint64_t(caps.flashBase) + caps.flashSize;
    uint64_t b = addr;
    uint64_t e = uint64_t(addr) + len;
    if (e <= flashLo || b >= flashHi) return;   // RAM/ZI declarations: ignored
    if (b < flashLo) b = flashLo;
    if (e > flashHi) e = flashHi;
    AddRange(g_ses.mappedFlash, b, e);
    AgdiLog::Write("PROGRAM", "[PROGRAM] memmap flash range 0x%08lX +%llu "
                   "recorded (part of the load footprint)",
                   (unsigned long)b, (unsigned long long)(e - b));
}

void ResetSession() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_ses.state == State::Active) {
        if (g_ses.token != 0 && AgdiSession::Usable()) {
            const bbx::RequestResult r =
                AgdiSession::Client().programAbort(g_ses.token);
            AgdiLog::Write("PROGRAM", "[PROGRAM] abort reason=session teardown "
                           "status=%lu (staging discarded, live flash unchanged)",
                           (unsigned long)r.status);
        } else {
            AgdiLog::Write("PROGRAM", "[PROGRAM] active transaction dropped "
                           "(no usable session; the server aborts on disconnect)");
        }
    }
    ClearLocked("session reset");
}

}  // namespace AgdiProgram