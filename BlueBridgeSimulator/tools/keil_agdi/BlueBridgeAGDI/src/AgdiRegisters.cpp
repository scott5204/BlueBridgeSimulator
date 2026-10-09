/*
 * Keil register view + AG_AllReg / AG_RegAcc adapter (stage 7-2B.2).
 *
 * Refresh path: ONE batch READ_REGISTERS for the whole view (section 47/56) --
 * never 17 pipe round trips. RegGet(UPR_NORMAL) is µVision's "re-read the
 * target" trigger; the per-item calls then only format the cached value, which
 * is what keeps the Registers window cheap while µVision repaints it.
 */

#include "AgdiRegisters.h"

#include <stdio.h>
#include <string.h>

#include <vector>

#include "AgdiLog.h"
#include "AgdiSession.h"

namespace {

/* --------------------------------------------------------------------------
 * register item table
 * ------------------------------------------------------------------------ */
enum ItemKind {
    kKindValue = 0,
    kKindXpsrBit = 1,   // derived from xPSR; write = read-modify-write
};

struct ItemDef {
    uint16_t item;      // RITEM::nItem (µVision hands this back in RegGet/RegSet)
    uint16_t group;
    const char *name;
    uint8_t isPC;
    uint8_t canChg;
    uint32_t wireId;
    uint8_t kind;
    uint32_t mask;      // kKindXpsrBit only
    uint8_t shift;      // kKindXpsrBit only
};

/* groups */
enum { kGrpCurrent = 0, kGrpXpsr = 1, kGrpStack = 2, kGrpFpu = 3, kGrpCount = 4 };

const char *kGroupNames[kGrpCount] = {"Current", "xPSR", "Stack & Control", "FPU"};
const uint8_t kGroupFlags[kGrpCount] = {0x01, 0x01, 0x01, 0x00};

/* items (order = display order) */
const ItemDef kItemDefs[] = {
    /* Current */
    {0x00, kGrpCurrent, "R0", 0, 1, bbipc::kWireRegR0, kKindValue, 0, 0},
    {0x01, kGrpCurrent, "R1", 0, 1, bbipc::kWireRegR1, kKindValue, 0, 0},
    {0x02, kGrpCurrent, "R2", 0, 1, bbipc::kWireRegR2, kKindValue, 0, 0},
    {0x03, kGrpCurrent, "R3", 0, 1, bbipc::kWireRegR3, kKindValue, 0, 0},
    {0x04, kGrpCurrent, "R4", 0, 1, bbipc::kWireRegR4, kKindValue, 0, 0},
    {0x05, kGrpCurrent, "R5", 0, 1, bbipc::kWireRegR5, kKindValue, 0, 0},
    {0x06, kGrpCurrent, "R6", 0, 1, bbipc::kWireRegR6, kKindValue, 0, 0},
    {0x07, kGrpCurrent, "R7", 0, 1, bbipc::kWireRegR7, kKindValue, 0, 0},
    {0x08, kGrpCurrent, "R8", 0, 1, bbipc::kWireRegR8, kKindValue, 0, 0},
    {0x09, kGrpCurrent, "R9", 0, 1, bbipc::kWireRegR9, kKindValue, 0, 0},
    {0x0A, kGrpCurrent, "R10", 0, 1, bbipc::kWireRegR10, kKindValue, 0, 0},
    {0x0B, kGrpCurrent, "R11", 0, 1, bbipc::kWireRegR11, kKindValue, 0, 0},
    {0x0C, kGrpCurrent, "R12", 0, 1, bbipc::kWireRegR12, kKindValue, 0, 0},
    {0x0D, kGrpCurrent, "R13 (SP)", 0, 1, bbipc::kWireRegSP, kKindValue, 0, 0},
    {0x0E, kGrpCurrent, "R14 (LR)", 0, 1, bbipc::kWireRegLR, kKindValue, 0, 0},
    {0x0F, kGrpCurrent, "R15 (PC)", 1, 1, bbipc::kWireRegPC, kKindValue, 0, 0},

    /* xPSR */
    {0x800, kGrpXpsr, "xPSR", 0, 1, bbipc::kWireRegXPSR, kKindValue, 0, 0},
    {0x801, kGrpXpsr, "N", 0, 1, bbipc::kWireRegXPSR, kKindXpsrBit, 0x80000000u, 31},
    {0x802, kGrpXpsr, "Z", 0, 1, bbipc::kWireRegXPSR, kKindXpsrBit, 0x40000000u, 30},
    {0x803, kGrpXpsr, "C", 0, 1, bbipc::kWireRegXPSR, kKindXpsrBit, 0x20000000u, 29},
    {0x804, kGrpXpsr, "V", 0, 1, bbipc::kWireRegXPSR, kKindXpsrBit, 0x10000000u, 28},
    {0x805, kGrpXpsr, "Q", 0, 1, bbipc::kWireRegXPSR, kKindXpsrBit, 0x08000000u, 27},
    {0x806, kGrpXpsr, "T", 0, 0, bbipc::kWireRegXPSR, kKindXpsrBit, 0x01000000u, 24},
    {0x807, kGrpXpsr, "ISR", 0, 0, bbipc::kWireRegXPSR, kKindXpsrBit, 0x000001FFu, 0},

    /* Stack & Control */
    {0x10, kGrpStack, "MSP", 0, 1, bbipc::kWireRegMSP, kKindValue, 0, 0},
    {0x11, kGrpStack, "PSP", 0, 1, bbipc::kWireRegPSP, kKindValue, 0, 0},
    {0x12, kGrpStack, "CONTROL", 0, 1, bbipc::kWireRegCONTROL, kKindValue, 0, 0},
    {0x13, kGrpStack, "PRIMASK", 0, 1, bbipc::kWireRegPRIMASK, kKindValue, 0, 0},
    {0x14, kGrpStack, "BASEPRI", 0, 1, bbipc::kWireRegBASEPRI, kKindValue, 0, 0},
    {0x15, kGrpStack, "FAULTMASK", 0, 1, bbipc::kWireRegFAULTMASK, kKindValue, 0, 0},
};

const size_t kValueItemCount = sizeof(kItemDefs) / sizeof(kItemDefs[0]);

/* FPU items are appended when the capability says so (section 55) */
const size_t kMaxItems = kValueItemCount + 34;   // S0-S31 + FPSCR + delimiter
struct rItem g_items[kMaxItems];
struct rGroup g_groups[kGrpCount];
size_t g_itemCount = 0;
bool g_installed = false;

pCBF g_pCb = NULL;
UL32 *g_pCurPc = NULL;

/* --------------------------------------------------------------------------
 * cache (one entry per wire register id)
 * ------------------------------------------------------------------------ */
const uint32_t kCacheMax = 96;
uint32_t g_cache[kCacheMax];
bool g_cacheValid[kCacheMax];
bool g_upToDate = false;

bool CacheGet(uint32_t wireId, uint32_t &out) {
    if (wireId >= kCacheMax || !g_cacheValid[wireId]) return false;
    out = g_cache[wireId];
    return true;
}

void CacheSet(uint32_t wireId, uint32_t v) {
    if (wireId >= kCacheMax) return;
    g_cache[wireId] = v;
    g_cacheValid[wireId] = true;
}

/* official RgARM block (AG_AllReg) */
RgARM g_rgArm;

/* AG_RegAcc register ids (values from the SDK's COLLECT.H, as used by the
 * official ARM sample driver; mPC comes from AGDI.H). */
const U32 kRegCpsr = 0x10;
const U32 kRegSpsr = 0x11;

/* FPU items are added to the lookup when the capability says so (section 55) */
ItemDef g_fpuDefs[33];
size_t g_fpuCount = 0;

const ItemDef *FindItem(uint16_t item) {
    for (size_t i = 0; i < kValueItemCount; ++i) {
        if (kItemDefs[i].item == item) return &kItemDefs[i];
    }
    for (size_t i = 0; i < g_fpuCount; ++i) {
        if (g_fpuDefs[i].item == item) return &g_fpuDefs[i];
    }
    return NULL;
}

void FormatValue(const ItemDef &d, uint32_t v, char *out, size_t n) {
    if (d.kind == kKindXpsrBit) {
        if (d.item == 0x807) {
            _snprintf_s(out, n, _TRUNCATE, "0x%03X", v);   // ISR number
        } else {
            _snprintf_s(out, n, _TRUNCATE, "%u", v);       // single bit
        }
        return;
    }
    _snprintf_s(out, n, _TRUNCATE, "0x%08X", v);
}

/* --------------------------------------------------------------------------
 * official RegGet / RegSet callbacks
 * ------------------------------------------------------------------------ */

void RegGet(RITEM *vp, int nR) {
    if (vp == NULL) return;

    /* µVision starts a refresh cycle with UPR_NORMAL: that is our cue to read
     * the target once (the following per-item calls only format the cache). */
    if (((unsigned)nR & (unsigned)UPR_NORMAL) != 0) {
        if (!g_upToDate) AgdiRegisters::Refresh();
        return;
    }

    const ItemDef *d = FindItem(vp->nItem);
    if (d == NULL) return;

    uint32_t v = 0;
    if (!CacheGet(d->wireId, v)) return;
    if (d->kind == kKindXpsrBit) v = (v & d->mask) >> d->shift;

    if (v != vp->v.u32 || vp->szVal[0] == 0) {
        FormatValue(*d, v, vp->szVal, sizeof(vp->szVal));
    }
    vp->v.u32 = v;
    vp->iDraw = 1;
}

I32 RegSet(RITEM *vp, GVAL *pV) {
    if (vp == NULL || pV == NULL) return 0;
    if (!AgdiSession::Usable()) return 0;

    const ItemDef *d = FindItem(vp->nItem);
    if (d == NULL || !d->canChg) return 0;

    uint32_t newVal = pV->u32;
    if (d->kind == kKindXpsrBit) {
        uint32_t cur = 0;
        if (!CacheGet(bbipc::kWireRegXPSR, cur)) return 0;
        newVal = (cur & ~d->mask) | ((newVal << d->shift) & d->mask);
    }

    const bbx::RequestResult r =
        AgdiSession::Client().writeRegister(d->wireId, newVal);
    if (!r.ok()) {
        AgdiLog::WriteRateLimited("RegSet", "write %s failed status=%u",
                                  d->name ? d->name : "?", r.status);
        if (r.lost()) AgdiSession::Usable();
        return 0;
    }
    CacheSet(d->wireId, newVal);
    if (d->wireId == bbipc::kWireRegPC && g_pCurPc != NULL) *g_pCurPc = newVal;
    g_upToDate = false;   // PC may have moved as a side effect
    return 1;
}

/* --------------------------------------------------------------------------
 * view construction
 * ------------------------------------------------------------------------ */

void AddItem(uint16_t item, uint16_t group, const char *name, uint8_t isPC,
             uint8_t canChg) {
    if (g_itemCount >= kMaxItems) return;
    struct rItem &it = g_items[g_itemCount++];
    memset(&it, 0, sizeof(it));
    it.desc = 0x01;
    it.nGi = group;
    it.nItem = item;
    _snprintf_s(it.szReg, sizeof(it.szReg), _TRUNCATE, "%s", name ? name : "");
    it.isPC = isPC;
    it.canChg = canChg;
}

void AddDelimiter(uint16_t item, uint16_t group) {
    AddItem(item, group, "", 0, 0);
}

void BuildView() {
    g_itemCount = 0;
    g_fpuCount = 0;
    for (size_t i = 0; i < kValueItemCount; ++i) {
        const ItemDef &d = kItemDefs[i];
        AddItem(d.item, d.group, d.name, d.isPC, d.canChg);
    }
    AddDelimiter(0x808, kGrpXpsr);
    if ((AgdiSession::Caps().flags & bbipc::kCapFpu) != 0) {
        static char names[32][16];
        for (int i = 0; i < 32; ++i) {
            _snprintf_s(names[i], sizeof(names[i]), _TRUNCATE, "S%d", i);
            AddItem((uint16_t)(0x20 + i), kGrpFpu, names[i], 0, 1);
            g_fpuDefs[g_fpuCount].item = (uint16_t)(0x20 + i);
            g_fpuDefs[g_fpuCount].group = kGrpFpu;
            g_fpuDefs[g_fpuCount].name = names[i];
            g_fpuDefs[g_fpuCount].isPC = 0;
            g_fpuDefs[g_fpuCount].canChg = 1;
            g_fpuDefs[g_fpuCount].wireId = bbipc::kWireRegS0 + (uint32_t)i;
            g_fpuDefs[g_fpuCount].kind = kKindValue;
            g_fpuDefs[g_fpuCount].mask = 0;
            g_fpuDefs[g_fpuCount].shift = 0;
            ++g_fpuCount;
        }
        AddItem(0x40, kGrpFpu, "FPSCR", 0, 1);
        g_fpuDefs[g_fpuCount].item = 0x40;
        g_fpuDefs[g_fpuCount].group = kGrpFpu;
        g_fpuDefs[g_fpuCount].name = "FPSCR";
        g_fpuDefs[g_fpuCount].isPC = 0;
        g_fpuDefs[g_fpuCount].canChg = 1;
        g_fpuDefs[g_fpuCount].wireId = bbipc::kWireRegFPSCR;
        g_fpuDefs[g_fpuCount].kind = kKindValue;
        g_fpuDefs[g_fpuCount].mask = 0;
        g_fpuDefs[g_fpuCount].shift = 0;
        ++g_fpuCount;
        AddDelimiter(0x41, kGrpFpu);
    }
}

}  // namespace

namespace AgdiRegisters {

void Install(void *pCbFunc, UL32 *pCurPc) {
    g_pCb = (pCBF)pCbFunc;
    g_pCurPc = pCurPc;
    if (g_pCb == NULL) {
        AgdiLog::Write("AgdiRegisters", "Install SKIPPED: no callback (AG_INITCALLBACK missing)");
        return;
    }

    BuildView();
    for (int i = 0; i < kGrpCount; ++i) {
        memset(&g_groups[i], 0, sizeof(g_groups[i]));
        g_groups[i].desc = 0x00;
        g_groups[i].ShEx = kGroupFlags[i];
        g_groups[i].name = (char *)kGroupNames[i];
    }
    memset(&g_rgArm, 0, sizeof(g_rgArm));
    memset(g_cacheValid, 0, sizeof(g_cacheValid));
    g_upToDate = false;

    struct RegDsc dsc;
    dsc.nGitems = (I32)kGrpCount;
    dsc.nRitems = (I32)g_itemCount;
    dsc.GrpArr = g_groups;
    dsc.RegArr = g_items;
    dsc.RegGet = &RegGet;
    dsc.RegSet = &RegSet;
    g_pCb(AG_CB_INITREGV, &dsc);

    g_installed = true;
    AgdiLog::Write("AgdiRegisters", "Installed RegView: %u groups, %u items "
                   "(FPU %s)",
                   (unsigned)kGrpCount, (unsigned)g_itemCount,
                   (AgdiSession::Caps().flags & bbipc::kCapFpu) ? "on" : "off");
    /* µVision refreshes on its own right after the install; seed the cache so
     * the very first paint already shows real values. */
    Refresh();
}

void Invalidate() { g_upToDate = false; }

void Refresh() {
    if (!AgdiSession::Usable()) {
        g_upToDate = false;
        return;
    }

    std::vector<uint32_t> ids;
    ids.push_back(bbipc::kWireRegPC);
    for (size_t i = 0; i < kValueItemCount; ++i) {
        const uint32_t id = kItemDefs[i].wireId;
        bool seen = false;
        for (size_t j = 0; j < ids.size(); ++j) {
            if (ids[j] == id) { seen = true; break; }
        }
        if (!seen) ids.push_back(id);
    }
    if ((AgdiSession::Caps().flags & bbipc::kCapFpu) != 0) {
        for (uint32_t id = bbipc::kWireRegS0; id <= bbipc::kWireRegS31; ++id) {
            ids.push_back(id);
        }
        ids.push_back(bbipc::kWireRegFPSCR);
    }

    std::vector<bbx::RegisterValue> vals;
    const bbx::RequestResult r = AgdiSession::Client().readRegisters(ids, &vals);
    if (!r.ok()) {
        AgdiLog::WriteRateLimited("AgdiRegisters", "refresh failed status=%u", r.status);
        if (r.lost()) AgdiSession::Usable();
        return;
    }

    size_t failed = 0;
    for (size_t i = 0; i < vals.size(); ++i) {
        if (vals[i].status == bbipc::kStOk) {
            CacheSet(vals[i].id, (uint32_t)vals[i].value);
        } else {
            ++failed;
        }
    }
    if (failed > 0) {
        AgdiLog::WriteRateLimited("AgdiRegisters", "%u/%u register reads rejected",
                                  (unsigned)failed, (unsigned)vals.size());
    }

    /* keep the official RgARM block (AG_AllReg) in sync */
    memset(&g_rgArm, 0, sizeof(g_rgArm));
    uint32_t v = 0;
    for (int i = 0; i < 16; ++i) {
        if (CacheGet(bbipc::kWireRegR0 + (uint32_t)i, v)) g_rgArm.cur[i] = v;
    }
    if (CacheGet(bbipc::kWireRegXPSR, v)) g_rgArm.cpsr = v;

    if (g_pCurPc != NULL && CacheGet(bbipc::kWireRegPC, v)) {
        *g_pCurPc = v;   // PC is passed through unchanged (section 53)
    }
    g_upToDate = true;
}

/* --------------------------------------------------------------------------
 * AG_RegAcc (single register, classic register ids -- section 45/46)
 * ------------------------------------------------------------------------ */

U32 RegAcc(U16 nCode, U32 nReg, GVAL *pV) {
    if (!AgdiSession::Usable()) return AG_NOACCESS;
    if (pV == NULL) return AG_INVALOP;

    uint32_t wire = bbipc::kWireRegInvalid;
    switch (nReg) {
        case 0x00: case 0x01: case 0x02: case 0x03:
        case 0x04: case 0x05: case 0x06: case 0x07:
        case 0x08: case 0x09: case 0x0A: case 0x0B:
        case 0x0C: case 0x0D: case 0x0E: case 0x0F:
            wire = bbipc::kWireRegR0 + nReg;
            break;
        case mPC:                       // 0x500 (AGDI.H)
            wire = bbipc::kWireRegPC;
            break;
        case kRegCpsr:                  // 0x10 -> Cortex-M xPSR
            wire = bbipc::kWireRegXPSR;
            break;
        case kRegSpsr:                  // 0x11: no SPSR on Cortex-M
            pV->u32 = 0;
            return AG_OK;
        default:
            AgdiLog::WriteRateLimited("AG_RegAcc", "unknown nReg=0x%08lX", (unsigned long)nReg);
            return AG_INVALOP;
    }

    if (nCode == AG_READ) {
        uint64_t v = 0;
        const bbx::RequestResult r = AgdiSession::Client().readRegister(wire, &v);
        if (!r.ok()) {
            AgdiLog::WriteRateLimited("AG_RegAcc", "read %s status=%u",
                                      bbx::registerName(wire), r.status);
            if (r.lost()) AgdiSession::Usable();
            return r.lost() ? AG_NOACCESS : AG_RDFAILED;
        }
        pV->u32 = (uint32_t)v;
        return AG_OK;
    }

    if (nCode == AG_WRITE) {
        const bbx::RequestResult r =
            AgdiSession::Client().writeRegister(wire, pV->u32);
        if (!r.ok()) {
            AgdiLog::WriteRateLimited("AG_RegAcc", "write %s status=%u",
                                      bbx::registerName(wire), r.status);
            if (r.lost()) AgdiSession::Usable();
            return r.lost() ? AG_NOACCESS : AG_WRFAILED;
        }
        CacheSet(wire, pV->u32);
        if (wire == bbipc::kWireRegPC && g_pCurPc != NULL) *g_pCurPc = pV->u32;
        g_upToDate = false;
        return AG_OK;
    }

    AgdiLog::WriteRateLimited("AG_RegAcc", "nCode=%u (unknown) nReg=0x%08lX -> "
                              "AG_INVALOP", (unsigned)nCode, (unsigned long)nReg);
    return AG_INVALOP;
}

/* --------------------------------------------------------------------------
 * AG_AllReg (official RgARM block -- section 57)
 *
 * nCode 0x1A / 0x1B (Cortex-M FPU block) are NOT in the 2003 sample header but
 * are sent by current µVision: CMSIS_AGDI copies exactly 0x21 dwords (132
 * bytes) for them -- 32 x 32-bit FPU registers s0..s31 followed by the FPU
 * status word at block offset 0x80 (its AG_RegAcc stores FPSCR there,
 * verified by disassembly). Refusing them with AG_INVALOP makes µVision abort
 * the running command with "*** error 123: AGDI: invalid operation".
 * ------------------------------------------------------------------------ */

const U16 kAllRegFpuRead  = 0x1A;
const U16 kAllRegFpuWrite = 0x1B;

U32 AllReg(U16 nCode, void *vp) {
    if (!AgdiSession::Usable()) return AG_NOACCESS;
    if (vp == NULL) return AG_INVALOP;

    if (nCode == AG_READ) {
        if (!g_upToDate) Refresh();
        memcpy(vp, &g_rgArm, sizeof(RgARM));
        return AG_OK;
    }

    if (nCode == AG_WRITE) {
        memcpy(&g_rgArm, vp, sizeof(RgARM));
        std::vector<bbx::RegisterValue> in;
        for (uint32_t i = 0; i < 16; ++i) {
            bbx::RegisterValue rv;
            rv.id = bbipc::kWireRegR0 + i;
            rv.value = g_rgArm.cur[i];
            in.push_back(rv);
        }
        bbx::RegisterValue psr;
        psr.id = bbipc::kWireRegXPSR;
        psr.value = g_rgArm.cpsr;
        in.push_back(psr);

        std::vector<bbx::RegisterValue> out;
        const bbx::RequestResult r =
            AgdiSession::Client().writeRegisters(in, &out);
        if (!r.ok()) {
            AgdiLog::WriteRateLimited("AG_AllReg", "write back failed status=%u", r.status);
            if (r.lost()) AgdiSession::Usable();
            return r.lost() ? AG_NOACCESS : AG_WRFAILED;
        }
        for (size_t i = 0; i < in.size(); ++i) CacheSet(in[i].id, (uint32_t)in[i].value);
        if (g_pCurPc != NULL) *g_pCurPc = g_rgArm.cur[15];
        g_upToDate = false;
        return AG_OK;
    }

    /* Cortex-M FPU register block: 32 x s0..s31 + FPSCR = 33 dwords (0x1A/0x1B).
     * The values come from the same cache Refresh() fills (it already fetches
     * the whole FPU set when the target reports FPU support). */
    if (nCode == kAllRegFpuRead) {
        if (!g_upToDate) Refresh();
        uint32_t *blk = (uint32_t *)vp;
        for (uint32_t i = 0; i < 32; ++i) {
            uint32_t v = 0;
            CacheGet(bbipc::kWireRegS0 + i, v);
            blk[i] = v;
        }
        uint32_t fs = 0;
        CacheGet(bbipc::kWireRegFPSCR, fs);
        blk[32] = fs;                       // block offset 0x80 (CMSIS_AGDI layout)
        return AG_OK;
    }

    if (nCode == kAllRegFpuWrite) {
        const uint32_t *blk = (const uint32_t *)vp;
        std::vector<bbx::RegisterValue> in;
        for (uint32_t i = 0; i < 32; ++i) {
            bbx::RegisterValue rv;
            rv.id = bbipc::kWireRegS0 + i;
            rv.value = blk[i];
            in.push_back(rv);
        }
        bbx::RegisterValue fs;
        fs.id = bbipc::kWireRegFPSCR;
        fs.value = blk[32];
        in.push_back(fs);

        std::vector<bbx::RegisterValue> out;
        const bbx::RequestResult r = AgdiSession::Client().writeRegisters(in, &out);
        if (!r.ok()) {
            AgdiLog::WriteRateLimited("AG_AllReg", "FPU block write failed status=%u",
                                      r.status);
            if (r.lost()) AgdiSession::Usable();
            return r.lost() ? AG_NOACCESS : AG_WRFAILED;
        }
        for (size_t i = 0; i < in.size(); ++i) CacheSet(in[i].id, (uint32_t)in[i].value);
        return AG_OK;
    }

    AgdiLog::WriteRateLimited("AG_AllReg", "nCode=%u (unknown) -> AG_INVALOP",
                              (unsigned)nCode);
    return AG_INVALOP;
}

}  // namespace AgdiRegisters