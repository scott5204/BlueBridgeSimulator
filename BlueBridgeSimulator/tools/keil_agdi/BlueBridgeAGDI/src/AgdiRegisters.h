#pragma once
/*
 * Keil register view + AG_AllReg / AG_RegAcc adapter (stage 7-2B.2 sections
 * 45-57 and 72-73).
 *
 * The values shown in µVision's Registers window come from the official
 * register-descriptor mechanism: we install a REGDSC through
 * pCbFunc(AG_CB_INITREGV, &dsc) and µVision pulls each item through
 * RegGet()/RegSet() -- exactly like Keil's AN173 sample driver does.
 *
 * There is NO second copy of the CPU state: every value is read from the IPC
 * target (one batch READ_REGISTERS per refresh), and writes go straight out as
 * WRITE_REGISTER. PC is passed through unchanged -- no PC+1 / PC|1 games
 * (section 53).
 */

#include "AgdiCompat.h"
#include "IpcTypes.h"

namespace AgdiRegisters {

/* Builds the descriptor from the validated capabilities, installs it into
 * µVision and drops the cache. Called from AG_Init(AG_INITFEATURES). */
void Install(void *pCbFunc, UL32 *pCurPc);

/* Marks the cached values stale (after step / reset / register write). */
void Invalidate();

/* Batch read + refresh of every register in the view; updates *pCURPC. */
void Refresh();

/* AGDI entry points (thin wrappers, mapped to AG_* error codes). */
U32 RegAcc(U16 nCode, U32 nReg, GVAL *pV);
U32 AllReg(U16 nCode, void *vp);

}  // namespace AgdiRegisters