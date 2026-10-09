#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "core/IBusDevice.h"

// Board-level watcher: receives a notification after every ODR/BSRR/MODER
// write so board devices (74LS573 LED latch, LCD control lines, ...) can
// react to pin changes exactly like real hardware wired to the pins.
class IGpioWatcher {
public:
    virtual ~IGpioWatcher() = default;
    // Called after any register write that may change pin levels/config.
    virtual void onGpioWrite(int portIndex) = 0;
};

// ============================================================================
// STM32G4 GPIO port model (RM0440 chapter "GPIO").
//
// Base addresses (AHB2):
//   GPIOA 0x48000000  GPIOB 0x48000400  GPIOC 0x48000800
//   GPIOD 0x48000C00  GPIOE 0x48001000
//
// Register offsets:
//   MODER 0x00  OTYPER 0x04  OSPEEDR 0x08  PUPDR 0x0C
//   IDR   0x10  ODR    0x14  BSRR    0x18  LCKR  0x1C
//   AFRL  0x20  AFRH   0x24  BRR     0x28
// ============================================================================
class GpioPort : public BusDevice32 {
public:
    explicit GpioPort(int index);

    void reset();

    // RCC AHB2ENR clock gate
    void setClockEnabled(bool en) { clockEnabled_ = en; }
    bool clockEnabled() const { return clockEnabled_; }

    void setWatcher(IGpioWatcher* w) { watcher_ = w; }

    // Called BEFORE the level report of a MODER/AFRL/AFRH write: the pin ->
    // peripheral routing just changed, so the SoC re-applies every peripheral
    // output to the pins (a pin switched to AF must immediately follow the
    // timer channel that was already running).
    void setAfRemapCallback(std::function<void()> fn) {
        afRemap_ = std::move(fn);
    }

    // ---- state inspection (board model) ----
    uint32_t odr() const { return odr_; }
    uint32_t moder() const { return moder_; }

    // Physical pin levels: output pins drive ODR, input/alt pins follow the
    // external drive (board devices) or the pull configuration.
    uint16_t pinLevels() const;

    // External input drive from board devices (buttons, signal generators).
    // 'mask' = pins driven, 'values' = levels for driven pins.
    void setExternalInput(uint16_t mask, uint16_t values) {
        setExternalInputAt(mask, values, nowCpu_);
    }

    // Same, but with the exact virtual cycle of the change. Board signal
    // sources (the virtual waveform generator) use this so pin observers --
    // the timer capture inputs and the board waveform monitor -- see the true
    // edge time instead of the end of the simulation batch.
    void setExternalInputAt(uint16_t mask, uint16_t values, uint64_t cpuCycle) {
        extMask_ = mask;
        extVal_ = values & mask;
        reportPinLevels(cpuCycle);
    }

    // ---- alternate-function (peripheral) pin drive ----
    // Called by the SoC when a peripheral (a timer channel) changes the level
    // it drives: the pin level then comes from the peripheral instead of ODR
    // (only while MODER = alternate function). The SoC owns the mapping from
    // peripheral signal to pin (see Stm32G431 AF table).
    void setAfPin(int pin, bool driven, bool level, uint64_t cpuCycle) {
        if (pin < 0 || pin >= 16) return;
        afDriven_ = (afDriven_ & ~(1u << pin)) | (driven ? (1u << pin) : 0);
        afLevel_ = (afLevel_ & ~(1u << pin)) | (level ? (1u << pin) : 0);
        reportPinLevels(cpuCycle);
    }

    // Pin level observers (timer capture inputs, board waveform monitors).
    // Fired on every effective level change with the exact virtual (CPU) cycle
    // of the change. setLevelObserver() replaces all observers of that pin
    // (the SoC AF router uses it); addLevelObserver() appends and is what the
    // board model uses, so it never disturbs the SoC routing.
    void setLevelObserver(int pin, std::function<void(bool, uint64_t)> fn) {
        if (pin < 0 || pin >= 16) return;
        levelObservers_[pin].clear();
        levelObservers_[pin].push_back(std::move(fn));
    }
    void addLevelObserver(int pin, std::function<void(bool, uint64_t)> fn) {
        if (pin < 0 || pin >= 16) return;
        levelObservers_[pin].push_back(std::move(fn));
    }

    // Virtual time reference for register-driven level changes (the Simulator
    // updates it once per batch; peripheral-driven changes pass their own).
    void setNowCycles(uint64_t cycles) {
        nowCpu_ = cycles;
        lastReportedLevels_ = pinLevels();  // no notification for time only
    }

    uint16_t afDriven() const { return afDriven_; }

    // Alternate function number configured for a pin (AFR[L|H], 0..15)
    int pinAf(int pin) const {
        const uint32_t afr = (pin < 8) ? afrl_ : afrh_;
        return int((afr >> ((pin & 7) * 4)) & 0xFu);
    }
    // Mode of a pin (0 = input, 1 = output, 2 = alternate, 3 = analog)
    int pinMode(int pin) const { return int((moder_ >> (pin * 2)) & 3u); }

protected:
    uint32_t readReg(uint32_t regOff) override;
    void writeReg(uint32_t regOff, uint32_t value) override;

private:
    // Compare the effective pin levels with the last reported ones and notify
    // observers for every changed pin.
    void reportPinLevels(uint64_t cpuCycle);
    int index_;
    bool clockEnabled_ = false;

    // reset values per RM0440 (G4: all pins in analog mode after reset)
    uint32_t moder_ = 0xFFFFFFFFu;
    uint32_t otyper_ = 0;
    uint32_t ospeedr_ = 0;
    uint32_t pupdr_ = 0;
    uint32_t odr_ = 0;
    uint32_t lckr_ = 0;
    uint32_t afrl_ = 0;
    uint32_t afrh_ = 0;

    uint16_t extMask_ = 0;
    uint16_t extVal_ = 0;

    // peripheral (alternate function) drive, resolved by the SoC AF table
    uint16_t afDriven_ = 0;   // pins currently driven by a peripheral
    uint16_t afLevel_ = 0;    // level driven on those pins
    uint16_t lastReportedLevels_ = 0;
    std::vector<std::function<void(bool, uint64_t)>> levelObservers_[16];
    uint64_t nowCpu_ = 0;

    IGpioWatcher* watcher_ = nullptr;
    std::function<void()> afRemap_;
};
