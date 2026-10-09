/*
 * Breakpoint adapter -- implementation (stage 7-2B checkpoint B.3).
 *
 * Threading: µVision calls the breakpoint entry points from its debugger
 * thread(s); the run waiter only ever calls AddTemporary/RemoveTemporary from
 * the Go thread. All registry access plus the backend round trip is serialized
 * by one mutex (`g_mutex`); the IPC round trip is bounded by the ordinary
 * 2 s RPC timeout, so no caller can block indefinitely -- and neither
 * AddTemporary/RemoveTemporary nor the notifications ever wait for a RUN to
 * finish (spec sections 9/10).
 */

#include "AgdiBreakpoints.h"

#include <mutex>
#include <vector>

#include "AgdiLog.h"
#include "AgdiSession.h"

namespace {

/* uVision "collect" attributes of an execution breakpoint (official
 * COLLECT.H: ATRX_BREAK / ATRX_BPDIS). AG_BpInfo(AG_BPQUERY) answers in THIS
 * space -- NOT the AGDI memory-map AG_ATR_* bits: the modern CMSIS_AGDI
 * driver returns `*pB & 0xF00` (verified by disassembly of its AG_BpInfo). */
const uint32_t kAtrxBreak = 0x00000400u;   // enabled execution breakpoint
const uint32_t kAtrxBpDis = 0x00000800u;   // disabled execution breakpoint

struct Entry {
    uint32_t address = 0;     // canonical (Thumb bit cleared)
    uint32_t handle = 0;      // µVision breakpoint number (0 = unknown)
    bool enabled = false;
    bool temporary = false;
};

std::mutex g_mutex;
std::vector<Entry> g_entries;

uint32_t CountEnabledLocked(uint32_t address) {
    uint32_t n = 0;
    for (size_t i = 0; i < g_entries.size(); ++i) {
        if (g_entries[i].address == address && g_entries[i].enabled) ++n;
    }
    return n;
}

/* Backend round trips. Never called with an unusable session (the callers
 * check first and report the loss). */
uint32_t BackendAdd(uint32_t address) {
    const bbx::RequestResult r = AgdiSession::Client().addBreakpoint(address);
    return r.status;
}

uint32_t BackendRemove(uint32_t address) {
    const bbx::RequestResult r = AgdiSession::Client().removeBreakpoint(address);
    return r.status;
}

/* Finds the first entry that matches address (+ handle when given). */
int FindLocked(uint32_t address, uint32_t handle, bool preferNonTemporary) {
    int fallback = -1;
    for (size_t i = 0; i < g_entries.size(); ++i) {
        const Entry &e = g_entries[i];
        if (e.address != address) continue;
        if (handle != 0 && e.handle != 0 && e.handle != handle) continue;
        if (!preferNonTemporary || !e.temporary) return (int)i;
        if (fallback < 0) fallback = (int)i;
    }
    return fallback;
}

/* enable/disable state change of an existing entry, with backend refcount. */
uint32_t SetEnabledLocked(size_t index, bool enabled) {
    Entry &e = g_entries[index];
    if (e.enabled == enabled) return bbipc::kStOk;
    const uint32_t before = CountEnabledLocked(e.address);
    if (enabled) {
        if (before == 0) {   // 0 -> 1: install in the backend first
            const uint32_t st = BackendAdd(e.address);
            if (st != bbipc::kStOk) {
                AgdiLog::Write("Bp", "ADD_BREAKPOINT 0x%08lX failed status=%lu -> "
                               "local state unchanged", (unsigned long)e.address,
                               (unsigned long)st);
                return st;
            }
        }
        e.enabled = true;
    } else {
        if (before == 1) {   // 1 -> 0: uninstall from the backend
            const uint32_t st = BackendRemove(e.address);
            if (st != bbipc::kStOk) {
                AgdiLog::Write("Bp", "REMOVE_BREAKPOINT 0x%08lX failed status=%lu -> "
                               "local state unchanged", (unsigned long)e.address,
                               (unsigned long)st);
                return st;
            }
        }
        e.enabled = false;
    }
    return bbipc::kStOk;
}

int CountEntriesLocked() {
    int n = 0;
    for (size_t i = 0; i < g_entries.size(); ++i) {
        if (!g_entries[i].temporary) ++n;
    }
    return n;
}

/* Distinct addresses with at least one enabled entry = installed backend
 * breakpoints (g_mutex must be held). */
uint32_t BackendCountLocked() {
    std::vector<uint32_t> seen;
    for (size_t i = 0; i < g_entries.size(); ++i) {
        if (!g_entries[i].enabled) continue;
        bool dup = false;
        for (size_t k = 0; k < seen.size(); ++k) {
            if (seen[k] == g_entries[i].address) { dup = true; break; }
        }
        if (!dup) seen.push_back(g_entries[i].address);
    }
    return (uint32_t)seen.size();
}

/* Link logic WITHOUT taking g_mutex -- callers must already hold it (µVision
 * may reach "set" / "enable-state" paths that are already inside the lock;
 * re-locking a non-recursive std::mutex aborts with a system_error). */
uint32_t NotifyLinkLocked(uint32_t address, bool enabled, uint32_t handle) {
    const uint32_t addr = AgdiBreakpoints::Canonical(address);
    if (!AgdiSession::Usable()) return bbx::kStConnectionLost;

    Entry e;
    e.address = addr;
    e.handle = handle;
    e.enabled = enabled;
    e.temporary = false;

    if (enabled && CountEnabledLocked(addr) == 0) {
        const uint32_t st = BackendAdd(addr);
        if (st != bbipc::kStOk) {
            AgdiLog::Write("Bp", "link 0x%08lX: ADD_BREAKPOINT failed status=%lu "
                           "-> entry not registered", (unsigned long)addr,
                           (unsigned long)st);
            return st;   // local state unchanged = backend state (section 42)
        }
    }
    g_entries.push_back(e);
    AgdiLog::Write("Bp", "link 0x%08lX handle=%lu enabled=%d -> backend=%u entries",
                   (unsigned long)addr, (unsigned long)handle, enabled ? 1 : 0,
                   BackendCountLocked());
    return bbipc::kStOk;
}

}  // namespace

namespace AgdiBreakpoints {

uint32_t Canonical(uint32_t address) { return address & ~1u; }

void ResetSession() {
    std::lock_guard<std::mutex> lk(g_mutex);
    const size_t n = g_entries.size();
    g_entries.clear();
    if (n != 0) {
        AgdiLog::Write("Bp", "session reset: %u local breakpoint entry(ies) dropped "
                       "(backend clears on disconnect)", (unsigned)n);
    }
}

uint32_t NotifyLink(uint32_t address, bool enabled, uint32_t handle) {
    std::lock_guard<std::mutex> lk(g_mutex);
    return NotifyLinkLocked(address, enabled, handle);
}

uint32_t NotifyUnlink(uint32_t address, uint32_t handle) {
    const uint32_t addr = Canonical(address);
    std::lock_guard<std::mutex> lk(g_mutex);
    const int idx = FindLocked(addr, handle, true);
    if (idx < 0) {
        AgdiLog::Write("Bp", "unlink 0x%08lX handle=%lu: no local entry",
                       (unsigned long)addr, (unsigned long)handle);
        return bbipc::kStOk;
    }
    const Entry removed = g_entries[(size_t)idx];
    const bool wasLastEnabled =
        removed.enabled && CountEnabledLocked(addr) == 1;
    if (wasLastEnabled && AgdiSession::Usable()) {
        const uint32_t st = BackendRemove(addr);
        if (st != bbipc::kStOk) {
            AgdiLog::Write("Bp", "unlink 0x%08lX: REMOVE_BREAKPOINT failed "
                           "status=%lu -> entry kept", (unsigned long)addr,
                           (unsigned long)st);
            return st;   // rollback: local list unchanged
        }
    }
    g_entries.erase(g_entries.begin() + idx);
    AgdiLog::Write("Bp", "unlink 0x%08lX handle=%lu -> backend=%u entries",
                   (unsigned long)addr, (unsigned long)removed.handle,
                   BackendCountLocked());
    return bbipc::kStOk;
}

uint32_t NotifyEnabled(uint32_t address, uint32_t handle, bool enabled) {
    const uint32_t addr = Canonical(address);
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!AgdiSession::Usable()) return bbx::kStConnectionLost;
    const int idx = FindLocked(addr, handle, true);
    if (idx < 0) {
        /* µVision says the breakpoint exists but we never saw the link
         * notification -- mirror it instead of silently ignoring the state. */
        AgdiLog::Write("Bp", "enable-state 0x%08lX handle=%lu enabled=%d: no local "
                       "entry -> registering it", (unsigned long)addr,
                       (unsigned long)handle, enabled ? 1 : 0);
        return NotifyLinkLocked(address, enabled, handle);
    }
    const uint32_t st = SetEnabledLocked((size_t)idx, enabled);
    if (st == bbipc::kStOk) {
        AgdiLog::Write("Bp", "enable-state 0x%08lX handle=%lu -> enabled=%d "
                       "backend=%u entries", (unsigned long)addr,
                       (unsigned long)handle, enabled ? 1 : 0, BackendCountLocked());
    }
    return st;
}

uint32_t NotifySet(uint32_t address) {
    const uint32_t addr = Canonical(address);
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!AgdiSession::Usable()) return bbx::kStConnectionLost;
    const int idx = FindLocked(addr, 0, true);
    if (idx < 0) {
        return NotifyLinkLocked(address, true, 0);
    }
    return SetEnabledLocked((size_t)idx, true);
}

uint32_t NotifyKill(uint32_t address) {
    return NotifyUnlink(address, 0);
}

uint32_t NotifyEnable(uint32_t address) { return NotifySet(address); }

uint32_t NotifyDisable(uint32_t address) {
    const uint32_t addr = Canonical(address);
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!AgdiSession::Usable()) return bbx::kStConnectionLost;
    const int idx = FindLocked(addr, 0, true);
    if (idx < 0) {
        AgdiLog::Write("Bp", "disable 0x%08lX: no local entry (ignored)",
                       (unsigned long)addr);
        return bbipc::kStOk;
    }
    return SetEnabledLocked((size_t)idx, false);
}

uint32_t DisableAll() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!AgdiSession::Usable()) return 0;
    uint32_t affected = 0;
    for (size_t i = 0; i < g_entries.size(); ++i) {
        Entry &e = g_entries[i];
        if (e.temporary || !e.enabled) continue;
        const uint32_t st = SetEnabledLocked(i, false);
        if (st != bbipc::kStOk) break;   // stop on the first failure
        ++affected;
    }
    AgdiLog::Write("Bp", "AG_BPDISALL -> %lu breakpoint(s) disabled",
                   (unsigned long)affected);
    return affected;
}

uint32_t KillAll() {
    std::lock_guard<std::mutex> lk(g_mutex);
    const uint32_t affected = (uint32_t)CountEntriesLocked();
    if (affected == 0) return 0;
    if (!AgdiSession::Usable()) return 0;
    const bbx::RequestResult r = AgdiSession::Client().clearBreakpoints();
    if (r.status != bbipc::kStOk) {
        AgdiLog::Write("Bp", "AG_BPKILLALL: CLEAR_BREAKPOINTS failed status=%lu -> "
                       "local state kept", (unsigned long)r.status);
        return 0;
    }
    g_entries.clear();
    AgdiLog::Write("Bp", "AG_BPKILLALL -> %lu breakpoint(s) killed",
                   (unsigned long)affected);
    return affected;
}

uint32_t AddTemporary(uint32_t address) {
    const uint32_t addr = Canonical(address);
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!AgdiSession::Usable()) return bbx::kStConnectionLost;
    if (CountEnabledLocked(addr) == 0) {
        const uint32_t st = BackendAdd(addr);
        if (st != bbipc::kStOk) {
            AgdiLog::Write("Bp", "temporary 0x%08lX: ADD_BREAKPOINT failed "
                           "status=%lu", (unsigned long)st);
            return st;
        }
    }
    Entry e;
    e.address = addr;
    e.handle = 0;
    e.enabled = true;
    e.temporary = true;
    g_entries.push_back(e);
    AgdiLog::Write("Bp", "temporary breakpoint 0x%08lX installed (backend=%u)",
                   (unsigned long)addr, BackendCountLocked());
    return bbipc::kStOk;
}

uint32_t RemoveTemporary(uint32_t address) {
    const uint32_t addr = Canonical(address);
    std::lock_guard<std::mutex> lk(g_mutex);
    int idx = -1;
    for (size_t i = 0; i < g_entries.size(); ++i) {
        if (g_entries[i].address == addr && g_entries[i].temporary) {
            idx = (int)i;
            break;
        }
    }
    if (idx < 0) return bbipc::kStOk;   // already gone -- never a ghost
    const bool wasLastEnabled = CountEnabledLocked(addr) == 1;
    if (wasLastEnabled && AgdiSession::Usable()) {
        const uint32_t st = BackendRemove(addr);
        if (st != bbipc::kStOk) {
            AgdiLog::Write("Bp", "temporary 0x%08lX: REMOVE_BREAKPOINT failed "
                           "status=%lu -> entry kept", (unsigned long)addr,
                           (unsigned long)st);
            return st;
        }
    }
    g_entries.erase(g_entries.begin() + idx);
    AgdiLog::Write("Bp", "temporary breakpoint 0x%08lX removed (backend=%u)",
                   (unsigned long)addr, BackendCountLocked());
    return bbipc::kStOk;
}

uint32_t AttributeBits(uint32_t address) {
    const uint32_t addr = Canonical(address);
    std::lock_guard<std::mutex> lk(g_mutex);
    bool present = false, enabled = false;
    for (size_t i = 0; i < g_entries.size(); ++i) {
        const Entry &e = g_entries[i];
        if (e.address != addr || e.temporary) continue;   // our temp bp is invisible
        present = true;
        if (e.enabled) enabled = true;
    }
    if (enabled) return kAtrxBreak;
    if (present) return kAtrxBpDis;
    return 0;
}

bool InstalledAt(uint32_t address) {
    const uint32_t addr = Canonical(address);
    std::lock_guard<std::mutex> lk(g_mutex);
    return CountEnabledLocked(addr) != 0;
}

uint32_t LogicalCount() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return (uint32_t)CountEntriesLocked();
}

uint32_t TemporaryCount() {
    std::lock_guard<std::mutex> lk(g_mutex);
    uint32_t n = 0;
    for (size_t i = 0; i < g_entries.size(); ++i) {
        if (g_entries[i].temporary) ++n;
    }
    return n;
}

uint32_t BackendCount() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return BackendCountLocked();
}

}  // namespace AgdiBreakpoints