#include "stm32/gpio/GpioPort.h"

#include <cstdio>

namespace {
// GPIO register offsets
constexpr uint32_t REG_MODER = 0x00;
constexpr uint32_t REG_OTYPER = 0x04;
constexpr uint32_t REG_OSPEEDR = 0x08;
constexpr uint32_t REG_PUPDR = 0x0C;
constexpr uint32_t REG_IDR = 0x10;
constexpr uint32_t REG_ODR = 0x14;
constexpr uint32_t REG_BSRR = 0x18;
constexpr uint32_t REG_LCKR = 0x1C;
constexpr uint32_t REG_AFRL = 0x20;
constexpr uint32_t REG_AFRH = 0x24;
constexpr uint32_t REG_BRR = 0x28;

inline uint32_t modeOf(uint32_t moder, int pin) {
    return (moder >> (pin * 2)) & 3u;
}
inline uint32_t pullOf(uint32_t pupdr, int pin) {
    return (pupdr >> (pin * 2)) & 3u;
}
}  // namespace

// GPIOA..GPIOE base addresses on the AHB2 bus
static constexpr uint32_t kGpioBase[5] = {0x48000000u, 0x48000400u,
                                          0x48000800u, 0x48000C00u,
                                          0x48001000u};

GpioPort::GpioPort(int index)
    : BusDevice32(kGpioBase[index]), index_(index) {}

void GpioPort::reset() {
    moder_ = 0xFFFFFFFFu;
    otyper_ = 0;
    ospeedr_ = 0;
    pupdr_ = 0;
    odr_ = 0;
    lckr_ = 0;
    afrl_ = 0;
    afrh_ = 0;
    extMask_ = 0;
    extVal_ = 0;
    afDriven_ = 0;
    afLevel_ = 0;
    lastReportedLevels_ = 0;
    nowCpu_ = 0;
    clockEnabled_ = false;
}

uint16_t GpioPort::pinLevels() const {
    uint16_t lv = 0;
    for (int pin = 0; pin < 16; pin++) {
        uint32_t mode = modeOf(moder_, pin);
        bool high = false;
        switch (mode) {
        case 0:  // input: external drive, else pull config
            if (extMask_ & (1u << pin)) {
                high = (extVal_ >> pin) & 1u;
            } else {
                uint32_t pull = pullOf(pupdr_, pin);
                high = (pull == 1);  // pull-up
            }
            break;
        case 2:  // alternate function: a peripheral or the board drives it
            if (afDriven_ & (1u << pin)) {
                high = (afLevel_ >> pin) & 1u;  // timer channel output
            } else if (extMask_ & (1u << pin)) {
                high = (extVal_ >> pin) & 1u;   // board signal generator
            } else {
                uint32_t pull = pullOf(pupdr_, pin);
                high = (pull == 1);
            }
            break;
        case 1:  // output
            high = (odr_ >> pin) & 1u;
            break;
        default:  // analog
            high = false;
            break;
        }
        if (high) lv |= uint16_t(1u << pin);
    }
    return lv;
}

void GpioPort::reportPinLevels(uint64_t cpuCycle) {
    const uint16_t lv = pinLevels();
    uint16_t changed = uint16_t(lv ^ lastReportedLevels_);
    lastReportedLevels_ = lv;
    if (!changed) return;
    for (int pin = 0; pin < 16 && changed; pin++) {
        if (!((changed >> pin) & 1u)) continue;
        changed &= uint16_t(~(1u << pin));
        for (const auto& obs : levelObservers_[pin]) {
            if (obs) obs((lv >> pin) & 1u, cpuCycle);
        }
    }
}

uint32_t GpioPort::readReg(uint32_t regOff) {
    if (!clockEnabled_) {
        // Peripheral clock disabled: registers read as 0 (like real silicon).
        return 0;
    }
    switch (regOff) {
    case REG_MODER: return moder_;
    case REG_OTYPER: return otyper_;
    case REG_OSPEEDR: return ospeedr_;
    case REG_PUPDR: return pupdr_;
    case REG_IDR: return pinLevels();
    case REG_ODR: return odr_;
    case REG_BSRR: return 0;  // write-only
    case REG_LCKR: return lckr_ & 0x0001FFFFu;
    case REG_AFRL: return afrl_;
    case REG_AFRH: return afrh_;
    case REG_BRR: return 0;  // write-only
    default: return 0;
    }
}

void GpioPort::writeReg(uint32_t regOff, uint32_t value) {
    if (!clockEnabled_) {
        return;  // writes to unclocked peripherals are dropped
    }
    bool levelsMayHaveChanged = false;
    bool routingChanged = false;
    switch (regOff) {
    case REG_MODER:
        moder_ = value;
        levelsMayHaveChanged = true;
        routingChanged = true;
        break;
    case REG_OTYPER: otyper_ = value & 0xFFFF; break;
    case REG_OSPEEDR: ospeedr_ = value; break;
    case REG_PUPDR: pupdr_ = value; levelsMayHaveChanged = true; break;
    case REG_ODR:
        odr_ = value & 0xFFFF;
        levelsMayHaveChanged = true;
        break;
    case REG_BSRR:
        // bits 0..15 set, bits 16..31 reset
        odr_ = (odr_ | (value & 0xFFFFu)) & (~(value >> 16) & 0xFFFFu);
        levelsMayHaveChanged = true;
        break;
    case REG_LCKR: lckr_ = value & 0x0001FFFFu; break;
    case REG_AFRL:
        afrl_ = value;
        levelsMayHaveChanged = true;
        routingChanged = true;
        break;
    case REG_AFRH:
        afrh_ = value;
        levelsMayHaveChanged = true;
        routingChanged = true;
        break;
    case REG_BRR:
        odr_ &= ~(value & 0xFFFFu);
        levelsMayHaveChanged = true;
        break;
    default: break;
    }
    if (watcher_) watcher_->onGpioWrite(index_);
    // Re-apply the peripheral (alternate function) drive BEFORE reporting the
    // levels: a pin switched to AF must immediately follow its timer channel
    // instead of briefly showing the pull/external level.
    if (routingChanged && afRemap_) afRemap_();
    if (levelsMayHaveChanged) reportPinLevels(nowCpu_);
}
