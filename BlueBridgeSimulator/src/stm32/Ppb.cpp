#include "stm32/Ppb.h"

// offsets relative to 0xE000E000
namespace {
constexpr uint32_t P_SYST_CSR = 0x010;
constexpr uint32_t P_SYST_RVR = 0x014;
constexpr uint32_t P_SYST_CVR = 0x018;
constexpr uint32_t P_SYST_CALIB = 0x01C;

constexpr uint32_t P_ISER = 0x100;
constexpr uint32_t P_ICER = 0x180;
constexpr uint32_t P_ISPR = 0x200;
constexpr uint32_t P_ICPR = 0x280;
constexpr uint32_t P_IPR = 0x400;

constexpr uint32_t P_CPUID = 0xD00;
constexpr uint32_t P_ICSR = 0xD04;
constexpr uint32_t P_VTOR = 0xD08;
constexpr uint32_t P_AIRCR = 0xD0C;
constexpr uint32_t P_SCR = 0xD10;
constexpr uint32_t P_CCR = 0xD14;
constexpr uint32_t P_SHPR = 0xD18;
constexpr uint32_t P_SHCSR = 0xD24;
constexpr uint32_t P_CPACR = 0xD88;
constexpr uint32_t P_DEMCR = 0xDFC;
constexpr uint32_t P_STIR = 0xF00;

constexpr uint32_t ICSR_PENDSVSET = 1u << 28;
constexpr uint32_t ICSR_PENDSVCLR = 1u << 27;
constexpr uint32_t ICSR_PENDSTSET = 1u << 26;
constexpr uint32_t ICSR_PENDSTCLR = 1u << 25;
constexpr uint32_t ICSR_NMIPENDSET = 1u << 31;
constexpr uint32_t AIRCR_VECTKEY = 0x05FAu << 16;
constexpr uint32_t AIRCR_SYSRESETREQ = 1u << 2;
}  // namespace

void Ppb::reset() {
    systick.reset();
    nvic.reset();
    vtor_ = 0;
    cpacr_ = 0;
    demcr_ = 0;
    aircr_ = 0x0000'F000u;
    scr_ = 0;
    ccr_ = 0;
    shcsr_ = 0;
    icsrPendMask_ = 0;
}

uint32_t Ppb::readReg(uint32_t regOff) {
    // --- SysTick ---
    if (regOff == P_SYST_CSR) return systick.readCsr();
    if (regOff == P_SYST_RVR) return systick.rvr();
    if (regOff == P_SYST_CVR) return systick.readCvr();
    if (regOff == P_SYST_CALIB) return 0;  // TENMS unknown

    // --- NVIC ---
    if (regOff >= P_ISER && regOff < P_ISER + 0x40)
        return nvic.iser((regOff - P_ISER) / 4);
    if (regOff >= P_ICER && regOff < P_ICER + 0x40)
        return nvic.iser((regOff - P_ICER) / 4);
    if (regOff >= P_ISPR && regOff < P_ISPR + 0x40)
        return nvic.ispr((regOff - P_ISPR) / 4);
    if (regOff >= P_ICPR && regOff < P_ICPR + 0x40)
        return nvic.ispr((regOff - P_ICPR) / 4);
    if (regOff >= P_IPR && regOff < P_IPR + 0x100) {
        // byte-addressed priority registers
        uint32_t word = 0;
        for (int i = 0; i < 4; i++) {
            int irq = int(regOff - P_IPR) + i;
            if (irq < NvicController::kMaxExternalIrq)
                word |= uint32_t(nvic.iprByte(irq)) << (8 * i);
        }
        return word;
    }

    // --- SCB: SHPR words (before the switch, they overlap no other case) ---
    if (regOff >= P_SHPR && regOff < P_SHPR + 12) {
        uint32_t word = 0;
        for (int i = 0; i < 4; i++) {
            int exc = 4 + int(regOff - P_SHPR) + i;
            if (exc <= 15) word |= uint32_t(nvic.shprByte(exc)) << (8 * i);
        }
        return word;
    }

    // --- SCB ---
    switch (regOff) {
    case P_CPUID: return 0x410FC241u;  // Cortex-M4 r0p1
    case P_ICSR: {
        int pending = nvic.highestPendingEnabledExternal();
        if (nvic.isPending(15) && (systick.csr() & SysTickTimer::TICKINT))
            if (pending == 0 || nvic.priority(15) < nvic.priority(pending))
                pending = 15;
        if (nvic.isPending(14))
            if (pending == 0 || nvic.priority(14) < nvic.priority(pending))
                pending = 14;
        return (uint32_t(nvic.activeException()) & 0x1FFu) |
               (uint32_t(pending) << 12);
    }
    case P_VTOR: return vtor_;
    case P_AIRCR: return (aircr_ & 0xFFFFu) | 0xFA050000u;
    case P_SCR: return scr_;
    case P_CCR: return ccr_ | (1u << 9) /*STKALIGN*/;
    case P_SHCSR: return shcsr_;
    case P_CPACR: return cpacr_;
    case P_DEMCR: return demcr_;
    default: return 0;
    }
    return 0;
}

void Ppb::writeReg(uint32_t regOff, uint32_t value) {
    // --- SysTick ---
    if (regOff == P_SYST_CSR) {
        systick.writeCsr(value);
        return;
    }
    if (regOff == P_SYST_RVR) {
        systick.writeRvr(value);
        return;
    }
    if (regOff == P_SYST_CVR) {
        systick.writeCvr(value);
        return;
    }

    // --- NVIC ---
    if (regOff >= P_ISER && regOff < P_ISER + 0x40) {
        nvic.iserWrite(int((regOff - P_ISER) / 4), value);
        return;
    }
    if (regOff >= P_ICER && regOff < P_ICER + 0x40) {
        nvic.icerWrite(int((regOff - P_ICER) / 4), value);
        return;
    }
    if (regOff >= P_ISPR && regOff < P_ISPR + 0x40) {
        nvic.isprWrite(int((regOff - P_ISPR) / 4), value);
        return;
    }
    if (regOff >= P_ICPR && regOff < P_ICPR + 0x40) {
        nvic.icprWrite(int((regOff - P_ICPR) / 4), value);
        return;
    }
    if (regOff >= P_IPR && regOff < P_IPR + 0x100) {
        for (int i = 0; i < 4; i++) {
            int irq = int(regOff - P_IPR) + i;
            if (irq < NvicController::kMaxExternalIrq)
                nvic.setPriorityByte(irq + 16,
                                     uint8_t(value >> (8 * i)));
        }
        return;
    }

    // --- SCB ---
    switch (regOff) {
    case P_ICSR:
        if (value & ICSR_PENDSTSET) nvic.pend(15);
        if (value & ICSR_PENDSTCLR) nvic.clearPend(15);
        if (value & ICSR_PENDSVSET) nvic.pend(14);
        if (value & ICSR_PENDSVCLR) nvic.clearPend(14);
        if (value & ICSR_NMIPENDSET) {
            if (nmiPended) nmiPended();
        }
        return;
    case P_VTOR:
        vtor_ = value & 0xFFFFFF00u;
        return;
    case P_AIRCR:
        if ((value & 0xFFFF0000u) == AIRCR_VECTKEY) {
            aircr_ = value & 0xFFFFu;
            if (value & AIRCR_SYSRESETREQ) {
                if (sysResetRequested) sysResetRequested();
            }
        }
        return;
    case P_SCR: scr_ = value & 7u; return;
    case P_CCR: ccr_ = value; return;
    case P_SHCSR: shcsr_ = value; return;
    case P_CPACR:
        cpacr_ = value & 0x00F00000u;
        if (cpacrChanged) cpacrChanged(cpacr_);
        return;
    case P_DEMCR: demcr_ = value; return;
    default: break;
    }

    if (regOff >= P_SHPR && regOff < P_SHPR + 12) {
        for (int i = 0; i < 4; i++) {
            int exc = 4 + int(regOff - P_SHPR) + i;
            if (exc <= 15)
                nvic.setPriorityByte(exc, uint8_t(value >> (8 * i)));
        }
        return;
    }

    // --- STIR ---
    if (regOff == P_STIR) {
        nvic.pend(int(value & 0xFFu) + 16);
        return;
    }
}
