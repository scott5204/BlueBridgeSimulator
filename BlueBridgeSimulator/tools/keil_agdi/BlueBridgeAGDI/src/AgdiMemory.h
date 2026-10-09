#pragma once
/*
 * AG_MemAcc / AG_MemAtt adapter (stage 7-2B.2 sections 58-63).
 *
 *   AG_READ / AG_RDOPC  -> READ_MEMORY   (chunked to <= 64 KiB, section 59)
 *   AG_WRITE / AG_WROPC -> WRITE_MEMORY  (RAM; flash stays Unsupported -> AG_RO)
 *   AG_F_*              -> not part of B.2 (no Application Download)
 *
 * Ordinary debugger memory writes must never reach the virtual flash: the flash
 * only changes through the PROGRAM_* transaction, which B.2 does not wire into
 * AGDI at all (section 52).
 */

#include "AgdiCompat.h"

namespace AgdiMemory {

U32 MemAcc(U16 nCode, UC8 *pB, GADR *pA, UL32 nMany);
U32 MemAtt(U16 nCode, UL32 nAttr, GADR *pA);

/* Official AGDI.H name of an AG_MemAcc nCode (all 24 constants, including the
 * codes this driver does not implement) -- used by the B.4.1 load trace so the
 * recorded µVision call sequence is readable. Never returns NULL. */
const char *MemCodeName(U16 nCode);

}  // namespace AgdiMemory