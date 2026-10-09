#pragma once
/*
 * Keil Application Load -> BlueBridge virtual flash programming
 * (stage 7-2B.4, checkpoint B.4.2, spec sections 5-43 / 67-79).
 *
 * Measured µVision 5.43 lifecycle (B.4.1):
 *
 *   AG_Init(AG_INITITEM|AG_INITSTARTLOAD, LOADPARMS*)   <- begin marker
 *     ... AG_MemAtt(AG_MEMMAP) declarations ...
 *     AG_MemAcc(AG_WROPC, addr, bytes, n)               <- the code itself, 1..n blocks
 *   AG_Init(AG_INITITEM|AG_INITENDLOAD, LOADPARMS*)     <- end marker
 *   AG_Init(AG_EXECITEM|AG_RESET)                       <- µVision's own reset
 *
 * This module translates exactly that lifecycle into the stage-7-2A virtual
 * flash transaction:
 *
 *   begin    -> HALT (confirm) -> PROGRAM_BEGIN  (token)
 *   WROPC    -> flash ranges: PROGRAM_ERASE (minimal, once per range) +
 *                             PROGRAM_WRITE (<= 64 KiB chunks)
 *               SRAM/CCM ranges: WRITE_MEMORY (never staged)
 *               alias / unknown: refused, never silently remapped
 *   end      -> PROGRAM_END (atomic commit + TCG invalidate + reset + halt)
 *   failure  -> PROGRAM_ABORT (live flash stays untouched)
 *
 * Hard rules (spec 4/12/13/14/17/33):
 *  - the ordinary debugger memory path (READ/WRITE_MEMORY, AG_WRITE) is NOT
 *    touched: flash through it stays Unsupported/AG_RO;
 *  - only an ACTIVE Application Load session may program, and only AG_WROPC
 *    inside it;
 *  - LOADPARMS.noCode=1 opens no transaction at all (symbols only);
 *  - no AXF/ELF/DWARF parsing anywhere: address + bytes come from µVision.
 */

#include "AgdiCompat.h"

namespace AgdiProgram {

/* Transaction open and healthy: AG_WROPC may program. */
bool Active();

/* The current load session failed: further AG_WROPC blocks must be refused
 * without opening a new transaction (spec section 34). Cleared by the next
 * STARTLOAD / session teardown. */
bool Failed();

/* 'Load about to start' (AG_INITITEM|AG_INITSTARTLOAD). Returns an official
 * AGDI error code; non-AG_OK is reported to µVision (spec section 10). */
U32 BeginLoad(const LOADPARMS *parms);

/* 'Load finished' (AG_INITITEM|AG_INITENDLOAD): commits an active, healthy
 * transaction (PROGRAM_END) and verifies the new vector table; a failed
 * session is reported and never committed. */
U32 EndLoad();

/* Body of AG_WROPC for an ACTIVE load session (address dispatch + staging).
 * Returns an official AGDI error code; any failure aborts the transaction. */
U32 WriteOpcodes(UL32 addr, const UC8 *data, UL32 many);

/* AG_MemAtt(AG_MEMMAP) declaration seen during a load: remember the flash part
 * so the erase covers the whole image footprint (no stale bytes of a previous,
 * larger image inside the programmed region -- spec sections 25-27). */
void NoteMappedRange(UL32 addr, UL32 len);

/* AG_UNINIT / new session: abort a still-open transaction (live flash stays
 * untouched when the session is gone) and clear every local trace of it. */
void ResetSession();

}  // namespace AgdiProgram