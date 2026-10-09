#pragma once
/*
 * Breakpoint adapter (stage 7-2B checkpoint B.3, spec sections 29-42 / 116-118).
 *
 * Only EXECUTION (code) breakpoints exist. µVision owns the source-level
 * breakpoint list and resolves a source line to a machine address itself; this
 * adapter only ever sees addresses (section 32).
 *
 * Two kinds of breakpoints are tracked:
 *   * "logical"   -- one entry per µVision breakpoint (AG_BpInfo / AG_BreakFunc
 *                    notification: linked / unlinked / enable state changed);
 *   * "temporary" -- the run-to-address breakpoint the driver itself installs
 *                    for AG_GoStep(AG_GOTILADR); always removed again, even on
 *                    error / user halt / fault / connection loss (section 48).
 *
 * The backend (`ADD_BREAKPOINT` / `REMOVE_BREAKPOINT` / `CLEAR_BREAKPOINTS`)
 * is refcounted PER ADDRESS: several logical breakpoints on the same address
 * (µVision temporary run-to-cursor + user breakpoint, duplicate entries) always
 * produce exactly ONE backend breakpoint (sections 34/35). Local state is only
 * committed after the IPC call succeeded, and rolled back on failure (section
 * 42) -- the driver never claims a breakpoint the simulator does not have.
 *
 * Addresses are stored and sent with the Thumb bit cleared (section 33); the
 * simulator's breakpoint engine uses the same rule (PROJECT_HANDOFF 7-1 §13).
 */

#include <stdint.h>

namespace AgdiBreakpoints {

/* ---------------------------------------------------------------------------
 * lifecycle
 * ------------------------------------------------------------------------- */

/* Drops every local entry WITHOUT touching the backend. Used when the IPC
 * session is gone (connection lost / AG_UNINIT): the simulator clears its own
 * breakpoints on disconnect (stage 7-2A), so no ghost state may survive into
 * the next session (sections 41 / 89). */
void ResetSession();

/* ---------------------------------------------------------------------------
 * µVision notifications (AG_BreakFunc / AG_BpInfo)
 * ------------------------------------------------------------------------- */

/* A µVision breakpoint was added to / removed from its list. Returns a bbx
 * status (bbipc::kStOk on success). */
uint32_t NotifyLink(uint32_t address, bool enabled, uint32_t handle);
uint32_t NotifyUnlink(uint32_t address, uint32_t handle);

/* The enable state of an existing µVision breakpoint changed (nCode=4). */
uint32_t NotifyEnabled(uint32_t address, uint32_t handle, bool enabled);

/* Direct AG_BpInfo notifications. */
uint32_t NotifySet(uint32_t address);       // AG_BPSET     -> enabled entry
uint32_t NotifyKill(uint32_t address);      // AG_BPKILL    -> remove entry
uint32_t NotifyEnable(uint32_t address);    // AG_BPENABLE  -> enable entry
uint32_t NotifyDisable(uint32_t address);   // AG_BPDISABLE -> disable entry

/* AG_BPDISALL / AG_BPKILLALL: returns the number of affected EXECUTION
 * breakpoints (official MBpInfo returns that count, section 43). */
uint32_t DisableAll();
uint32_t KillAll();

/* ---------------------------------------------------------------------------
 * temporary (run-to-address) breakpoints
 * ------------------------------------------------------------------------- */
uint32_t AddTemporary(uint32_t address);
uint32_t RemoveTemporary(uint32_t address);

/* ---------------------------------------------------------------------------
 * queries
 * ------------------------------------------------------------------------- */

/* Official AG_BpInfo(AG_BPQUERY / AG_BPEXQUERY) answer: the uVision "collect"
 * attribute bits of that address -- ATRX_BREAK 0x400 (enabled execution
 * breakpoint) / ATRX_BPDIS 0x800 (disabled execution breakpoint), per the
 * official COLLECT.H. NOTE these are NOT the AGDI memory-map AG_ATR_* bits
 * (AGDI.H 0x08 / 0x40): the modern CMSIS_AGDI driver answers
 * `*pB & 0xF00` for AG_BPQUERY, verified by disassembly.
 * A "disabled" answer means: a logical breakpoint exists but is not installed
 * in the backend. */
uint32_t AttributeBits(uint32_t address);

/* true when an installed (enabled) backend breakpoint exists at the address. */
bool InstalledAt(uint32_t address);

/* Diagnostics */
uint32_t LogicalCount();
uint32_t BackendCount();
uint32_t TemporaryCount();

/* Normalises an address to the project-wide breakpoint rule (Thumb bit clear). */
uint32_t Canonical(uint32_t address);

}  // namespace AgdiBreakpoints