/*
 * AG_MemAcc / AG_MemAtt adapter (stage 7-2B.2 sections 58-63).
 */

#include "AgdiMemory.h"

#include <stdlib.h>
#include <string.h>

#include <vector>

#include "AgdiLog.h"
#include "AgdiProgram.h"
#include "AgdiSession.h"

namespace {

const UL32 kChunk = bbipc::kMaxMemoryTransfer;   // 64 KiB per pipe request

/* Memory attributes as they come out of the GET_CAPABILITIES memory map
 * (never invented per address: flash is read/exec only, RAM is read/write). */
UL32 AttributeOf(UL32 addr) {
    const bbx::TargetCaps &c = AgdiSession::Caps();
    if (addr >= c.flashBase && addr < c.flashBase + c.flashSize) {
        return AG_ATR_EXEC | AG_ATR_READ | AG_ATR_THUMB;
    }
    if (addr >= c.flashAliasBase && addr < c.flashAliasBase + c.flashAliasSize) {
        return AG_ATR_EXEC | AG_ATR_READ | AG_ATR_THUMB;
    }
    if (addr >= c.sramBase && addr < c.sramBase + c.sramSize) {
        return AG_ATR_READ | AG_ATR_WRITE;
    }
    if (addr >= c.ccmBase && addr < c.ccmBase + c.ccmSize) {
        return AG_ATR_READ | AG_ATR_WRITE;
    }
    if (addr >= 0x40000000u && addr < 0x60000000u) {
        return AG_ATR_READ | AG_ATR_WRITE;   // MMIO
    }
    if (addr >= 0xE0000000u) {
        return AG_ATR_READ | AG_ATR_WRITE;   // system / PPB
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Attribute arrays (official MapMemory / GetAttr model)
 *
 * µVision 5.x hands AG_GETMEMATT's result straight to its CPU DLL (SarmCM3),
 * which DEREFERENCES it: the official CMSIS_AGDI returns
 * `attrArray + ((addr & 0xFFFF) >> 2) * 4`, one 32-bit attribute word per
 * 4 bytes of each 64 KiB segment. Returning a plain attribute *bitmask* here
 * (as the ancient M166 sample did) makes SarmCM3 dereference garbage and
 * crash µVision, so the modern pointer contract is implemented.
 * ------------------------------------------------------------------------ */
const int kMaxSegments = 32;                  // lazily created 64 KiB blocks
const int kDwordsPerSeg = 0x10000 / 4;

struct Segment {
    uint32_t seg;
    uint32_t *block;                          // attribute words for the segment
};

Segment g_segments[kMaxSegments];
int g_segmentCount = 0;
uint32_t g_fallbackBlock[kDwordsPerSeg];      // used once kMaxSegments is hit

void FillSegmentBlock(uint32_t *block, uint32_t seg) {
    for (int i = 0; i < kDwordsPerSeg; ++i) {
        block[i] = AttributeOf((seg << 16) + (uint32_t)i * 4);
    }
}

uint32_t *SegmentBlock(uint32_t seg, bool create) {
    for (int i = 0; i < g_segmentCount; ++i) {
        if (g_segments[i].seg == seg) return g_segments[i].block;
    }
    if (!create) return NULL;
    if (g_segmentCount >= kMaxSegments) return g_fallbackBlock;
    uint32_t *block = (uint32_t *)malloc((size_t)kDwordsPerSeg * sizeof(uint32_t));
    if (block == NULL) return g_fallbackBlock;
    FillSegmentBlock(block, seg);
    g_segments[g_segmentCount].seg = seg;
    g_segments[g_segmentCount].block = block;
    ++g_segmentCount;
    return block;
}

/* attribute word for one address (pointer handed to µVision/SarmCM3) */
uint32_t *AttrDwordFor(UL32 addr, bool create) {
    uint32_t *block = SegmentBlock(addr >> 16, create);
    if (block == NULL) return NULL;
    return &block[(addr & 0xFFFFu) >> 2];
}

}  // namespace

namespace AgdiMemory {

/* Every official AG_MemAcc nCode from AGDI.H (0x01-0x19). The names matter for
 * the B.4.1 trace: whatever µVision sends during an application load must be
 * recorded by its official name, not guessed. */
const char *MemCodeName(U16 nCode) {
    switch (nCode) {
        case AG_READ:        return "AG_READ";
        case AG_WRITE:       return "AG_WRITE";
        case AG_WROPC:       return "AG_WROPC";
        case AG_RDOPC:       return "AG_RDOPC";
        case AG_RDMMU66:     return "AG_RDMMU66";
        case AG_WRMMU66:     return "AG_WRMMU66";
        case AG_RCRC:        return "AG_RCRC";
        case AG_RDSHIELD:    return "AG_RDSHIELD";
        case AG_RDACE:       return "AG_RDACE";
        case AG_F_WRITE:     return "AG_F_WRITE";
        case AG_F_VERIFY:    return "AG_F_VERIFY";
        case AG_F_ERASE:     return "AG_F_ERASE";
        case AG_F_RUN:       return "AG_F_RUN";
        case AG_RD_XDES:     return "AG_RD_XDES";
        case AG_RD_XAES:     return "AG_RD_XAES";
        case AG_RD_XCRC:     return "AG_RD_XCRC";
        case AG_RD_XVECT:    return "AG_RD_XVECT";
        case AG_RDRF:        return "AG_RDRF";
        case AG_WRRF:        return "AG_WRRF";
        case AG_RDMOVB:      return "AG_RDMOVB";
        case AG_WRMOVB:      return "AG_WRMOVB";
        case AG_BONVMREAD:   return "AG_BONVMREAD";
        case AG_BONVMWRITE:  return "AG_BONVMWRITE";
        default:             return "AG_Mem(unknown)";
    }
}

U32 MemAcc(U16 nCode, UC8 *pB, GADR *pA, UL32 nMany) {
    if (!AgdiSession::Usable()) return AG_NOACCESS;
    if (pA == NULL || pB == NULL) {
        AgdiLog::WriteRateLimited("AG_MemAcc", "nCode=%u with NULL %s -> AG_INVALOP",
                                  (unsigned)nCode,
                                  pA == NULL ? "GADR" : "buffer");
        return AG_INVALOP;
    }
    if (nMany == 0) return AG_OK;

    const UL32 base = pA->Adr;
    pA->ErrAdr = 0;

    if (nCode == AG_READ || nCode == AG_RDOPC) {
        UL32 done = 0;
        while (done < nMany) {
            const UL32 chunk = (nMany - done < kChunk) ? (nMany - done) : kChunk;
            std::vector<uint8_t> buf;
            const bbx::RequestResult r =
                AgdiSession::Client().readMemory(base + done, chunk, &buf);
            if (!r.ok()) {
                pA->ErrAdr = base + done;
                AgdiLog::WriteRateLimited("AG_MemAcc", "READ 0x%08lX +%lu failed status=%u",
                                          (unsigned long)(base + done),
                                          (unsigned long)chunk, r.status);
                if (r.lost()) { AgdiSession::Usable(); return AG_NOACCESS; }
                return AG_RDFAILED;
            }
            memcpy(pB + done, buf.data(), chunk);
            done += chunk;
        }
        AgdiLog::WriteRateLimited("AG_MemAcc", "READ 0x%08lX +%lu ok", (unsigned long)base,
                                  (unsigned long)nMany);
        return AG_OK;
    }

    if (nCode == AG_WROPC) {
        /* Application Download takeover (B.4.2, spec sections 13/14): only an
         * ACTIVE Application Load session may program the virtual flash, and
         * only through this nCode. Every other AG_WROPC (memory window, plain
         * debug writes) keeps the B.2/B.3 answer -- flash stays read-only. */
        if (AgdiProgram::Failed()) {
            AgdiLog::Write("AG_MemAcc", "AG_WROPC 0x%08lX +%lu refused: the "
                           "application-load session failed (no new transaction)",
                           (unsigned long)base, (unsigned long)nMany);
            return AG_WRFAILED;
        }
        if (AgdiProgram::Active()) {
            return AgdiProgram::WriteOpcodes(base, pB, nMany);
        }
    }

    if (nCode == AG_WRITE || nCode == AG_WROPC) {
        UL32 done = 0;
        while (done < nMany) {
            const UL32 chunk = (nMany - done < kChunk) ? (nMany - done) : kChunk;
            const bbx::RequestResult r = AgdiSession::Client().writeMemory(
                base + done, pB + done, chunk);
            if (!r.ok()) {
                pA->ErrAdr = base + done;
                /* writes are evidence, never rate limited (B.4.2 section 61):
                 * a burst of feature probing must not hide the fact that a
                 * flash write was refused */
                AgdiLog::Write("AG_MemAcc",
                               "WRITE %s 0x%08lX +%lu rejected status=%u -> %s",
                               MemCodeName(nCode), (unsigned long)(base + done),
                               (unsigned long)chunk, r.status,
                               r.status == bbipc::kStUnsupported ? "AG_RO" : "AG_WRFAILED");
                if (r.lost()) { AgdiSession::Usable(); return AG_NOACCESS; }
                /* Flash stays read-only for the ordinary memory-write path
                 * (section 52/60): report the official read-only error. */
                if (r.status == bbipc::kStUnsupported) return AG_RO;
                return AG_WRFAILED;
            }
            done += chunk;
        }
        AgdiLog::Write("AG_MemAcc", "WRITE %s 0x%08lX +%lu ok", MemCodeName(nCode),
                       (unsigned long)base, (unsigned long)nMany);
        return AG_OK;
    }

    if (nCode == AG_F_WRITE || nCode == AG_F_VERIFY || nCode == AG_F_ERASE ||
        nCode == AG_F_RUN) {
        /* Flash download / Application Load belongs to a later stage. */
        AgdiLog::Write("AG_MemAcc", "%s not supported in B.2 (no PROGRAM_* path)",
                       MemCodeName(nCode));
        return AG_INVALOP;
    }

    AgdiLog::WriteRateLimited("AG_MemAcc", "%s -> AG_INVALOP", MemCodeName(nCode));
    return AG_INVALOP;
}

U32 MemAtt(U16 nCode, UL32 nAttr, GADR *pA) {
    if (!AgdiSession::Usable()) return AG_NOACCESS;
    if (pA == NULL) {
        AgdiLog::WriteRateLimited("AG_MemAtt", "nCode=%u with NULL GADR -> AG_INVALOP",
                                  (unsigned)nCode);
        return AG_INVALOP;
    }

    switch (nCode) {
        case AG_MEMMAP: {
            /* official MapMemory(): allocate the 64 KiB attribute blocks for
             * the range and either clear them (nAttr == 0) or OR the requested
             * attributes in. pA->ErrAdr reports the result (0 = OK). */
            const UL32 addr = pA->Adr;
            UL32 len = pA->nLen;
            /* B.4.2: remember the flash part of µVision's load map -- the erase
             * plan covers the declared image footprint (spec section 27). */
            if (nAttr != 0) AgdiProgram::NoteMappedRange(addr, len);
            UL32 touched = 0;
            for (UL32 a = addr & ~3u; a < addr + len; a += 4) {
                uint32_t *p = AttrDwordFor(a, true);
                if (p == NULL) break;
                if (nAttr == 0) {
                    *p = 0;
                } else {
                    *p |= nAttr;
                }
                ++touched;
            }
            pA->ErrAdr = 0;
            AgdiLog::WriteRateLimited("AG_MemAtt",
                                      "AG_MEMMAP 0x%08lX +%lu attr=0x%08lX (%lu words)",
                                      (unsigned long)addr, (unsigned long)len,
                                      (unsigned long)nAttr, (unsigned long)touched);
            return AG_OK;
        }

        case AG_GETMEMATT: {
            /* modern contract (matches CMSIS_AGDI's GetAttr): the attribute
             * word POINTER for this address, dereferenced by µVision's CPU DLL */
            const UL32 addr = pA->Adr;
            uint32_t *attr = AttrDwordFor(addr, true);
            pA->Adr = (UL32)(uintptr_t)attr;
            AgdiLog::WriteRateLimited("AG_MemAtt", "AG_GETMEMATT 0x%08lX -> attr=0x%08lX",
                                      (unsigned long)addr,
                                      (unsigned long)(attr != NULL ? *attr : 0));
            return AG_OK;
        }

        case AG_SETMEMATT:
            /* official driver: no-op (falls through to the default case) */
            return AG_OK;

        default:
            AgdiLog::WriteRateLimited("AG_MemAtt", "nCode=%u (unknown) adr=0x%08lX "
                                      "-> AG_INVALOP", (unsigned)nCode,
                                      (unsigned long)pA->Adr);
            return AG_INVALOP;
    }
}

}  // namespace AgdiMemory