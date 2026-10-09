#include "stm32/rcc/Rcc.h"

#include <cstring>

// RCC register offsets (verified against ST CMSIS stm32g431xx.h)
namespace {
constexpr uint32_t R_CR = 0x00;
constexpr uint32_t R_ICSCR = 0x04;
constexpr uint32_t R_CFGR = 0x08;
constexpr uint32_t R_PLLCFGR = 0x0C;
constexpr uint32_t R_AHB1ENR = 0x48;
constexpr uint32_t R_AHB2ENR = 0x4C;
constexpr uint32_t R_AHB3ENR = 0x50;
constexpr uint32_t R_APB1ENR1 = 0x58;
constexpr uint32_t R_APB1ENR2 = 0x5C;
constexpr uint32_t R_APB2ENR = 0x60;
constexpr uint32_t R_CRRCR = 0x98;
constexpr uint32_t R_CCIPR2 = 0x9C;

// CR bits
constexpr uint32_t CR_HSION = 1u << 0;
constexpr uint32_t CR_HSIRDY = 1u << 1;
constexpr uint32_t CR_HSEON = 1u << 16;
constexpr uint32_t CR_HSERDY = 1u << 17;
constexpr uint32_t CR_PLLON = 1u << 24;
constexpr uint32_t CR_PLLRDY = 1u << 25;
}  // namespace

void Rcc::reset() {
    std::memset(regs_, 0, sizeof(regs_));
    // reset values: HSI on (CR = 0x00000063 on real silicon; the important
    // bits are HSION|HSIRDY which we synthesize on read)
    cr_ = CR_HSION;
    cfgr_ = 0;
    pllcfgr_ = 0;
    ahb2enr_ = 0;
}

uint32_t Rcc::readReg(uint32_t regOff) {
    switch (regOff) {
    case R_CR:
        // ready flags track enable flags (oscillators lock instantly in the
        // model - firmware polling loops complete)
        return cr_ | (cr_ & CR_HSION ? CR_HSIRDY : 0u) |
               (cr_ & CR_HSEON ? CR_HSERDY : 0u) |
               (cr_ & CR_PLLON ? CR_PLLRDY : 0u);
    case R_ICSCR: return regs_[R_ICSCR / 4];
    case R_CFGR:
        // SWS follows SW immediately after the switch write
        return (cfgr_ & ~0xCu) | ((cfgr_ & 0x3u) << 2);
    case R_PLLCFGR: return pllcfgr_;
    case R_AHB1ENR: return regs_[R_AHB1ENR / 4];
    case R_AHB2ENR: return ahb2enr_;
    case R_AHB3ENR: return regs_[R_AHB3ENR / 4];
    case R_APB1ENR1: return regs_[R_APB1ENR1 / 4];
    case R_APB1ENR2: return regs_[R_APB1ENR2 / 4];
    case R_APB2ENR: return regs_[R_APB2ENR / 4];
    case R_CRRCR: return regs_[R_CRRCR / 4] | (1u << 8);  // CRSRDY
    case R_CCIPR2: return regs_[R_CCIPR2 / 4];
    default: return regs_[regOff / 4];
    }
}

void Rcc::writeReg(uint32_t regOff, uint32_t value) {
    switch (regOff) {
    case R_CR:
        cr_ = value;
        break;
    case R_CFGR:
        cfgr_ = value;
        break;
    case R_PLLCFGR:
        pllcfgr_ = value;
        break;
    case R_AHB2ENR:
        ahb2enr_ = value;
        if (onAhb2enrChanged) onAhb2enrChanged();
        break;
    case R_APB2ENR:
        regs_[R_APB2ENR / 4] = value;
        if (onApb2enrChanged) onApb2enrChanged();
        break;
    default:
        if (regOff < kSize) regs_[regOff / 4] = value;
        break;
    }
}

uint32_t Rcc::debugReadReg(uint32_t regOff) const {
    // readReg is side-effect free
    return const_cast<Rcc*>(this)->readReg(regOff);
}

// ---------------------------------------------------------------------------
// Clock tree
// ---------------------------------------------------------------------------
uint32_t Rcc::sysclkHz() const {
    const uint32_t sw = cfgr_ & 0x3u;
    switch (sw) {
    case 2:  // HSE
        return (cr_ & CR_HSEON) ? kHseHz : 0;
    case 3: {  // PLL
        if (!(cr_ & CR_PLLON)) return 0;
        const uint32_t src = pllcfgr_ & 0x3u;
        uint64_t inHz = 0;
        if (src == 0x2u)
            inHz = kHsiHz;  // HSI16
        else if (src == 0x3u)
            inHz = kHseHz;  // HSE
        else
            return 0;
        const uint32_t m = ((pllcfgr_ >> 4) & 0xFu) + 1u;   // /M
        const uint32_t n = (pllcfgr_ >> 8) & 0x7Fu;         // *N
        const uint32_t rField = (pllcfgr_ >> 25) & 0x3u;    // R
        const uint32_t rDiv = (rField + 1u) * 2u;           // 2,4,6,8
        if (n == 0) return 0;
        uint64_t vco = inHz / m * n;
        return uint32_t(vco / rDiv);
    }
    default:  // 0/1: HSI16
        return (cr_ & CR_HSION) ? kHsiHz : 0;
    }
}

static uint32_t ahbDiv(uint32_t hpre) {
    if (hpre < 8) return 1;
    switch (hpre) {
    case 8: return 2;
    case 9: return 4;
    case 10: return 8;
    case 11: return 16;
    case 12: return 64;
    case 13: return 128;
    case 14: return 256;
    default: return 512;
    }
}

static uint32_t apbDiv(uint32_t ppre) {
    switch (ppre & 0x7u) {
    case 4: return 2;
    case 5: return 4;
    case 6: return 8;
    case 7: return 16;
    default: return 1;
    }
}

uint32_t Rcc::ahbHz() const {
    return sysclkHz() / ahbDiv((cfgr_ >> 4) & 0xFu);
}

uint32_t Rcc::apb1Hz() const {
    return ahbHz() / apbDiv((cfgr_ >> 8) & 0x7u);
}

uint32_t Rcc::apb2Hz() const {
    return ahbHz() / apbDiv((cfgr_ >> 11) & 0x7u);
}

uint32_t Rcc::apb1TimerHz() const {
    uint32_t d = apbDiv((cfgr_ >> 8) & 0x7u);
    return d == 1 ? apb1Hz() : apb1Hz() * 2;
}

uint32_t Rcc::apb2TimerHz() const {
    uint32_t d = apbDiv((cfgr_ >> 11) & 0x7u);
    return d == 1 ? apb2Hz() : apb2Hz() * 2;
}
