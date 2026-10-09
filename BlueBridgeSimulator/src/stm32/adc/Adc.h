#pragma once

#include <cstdint>
#include <functional>

#include "core/IBusDevice.h"

// ============================================================================
// STM32G4 ADC model (RM0440 chapter "Analog-to-digital converter"), stage 4.
//
// MMIO bases (STM32G431, docs/stm32g431xx.h):
//   ADC1          0x50000000   ADC2  0x50000100   ADC12_COMMON 0x50000300
//
// Implemented (enough for the competition use: two channels on two ADCs):
//   ISR  ADRDY / EOC / EOS / OVR (read DR clears EOC+EOS, OVR is W1C)
//   IER  ADRDYIE / EOCIE / EOSIE / OVRIE  (EOCIE -> ADC1_2_IRQn = 18)
//   CR   ADEN / ADDIS / ADSTART / ADSTP / ADCAL (+ ADVREGEN/DEEPPWD stored)
//   CFGR RES (12/10/8/6 bit), CONT, ALIGN, OVRMOD, DMAEN (stored)
//   SMPR1/SMPR2 sampling times (stored -- the model converts instantly)
//   SQR1..4 regular sequence (rank 1 of SQR1 selects the channel)
//   DR   regular data register (right or left aligned per CFGR.ALIGN)
//   JSQR/JDR1..4, TR1..3, OFR1..4, AWD2CR/AWD3CR, DIFSEL, CALFACT (stored)
//
// The conversion value comes from the BOARD: the SoC installs a channel ->
// voltage provider (R38 -> ADC1_IN11, R37 -> ADC2_IN15 on the CT117E-M4), so
// the ADC itself knows nothing about potentiometers. Conversion is
// instantaneous in virtual time, which is what a polling HAL firmware sees
// anyway (sampling + SAR time is ~1 us, far below the batch granularity).
//
// Not implemented: injected channels (conversion), analog watchdog action,
// oversampling, differential mode, DMA request generation, scan of several
// ranks (rank 1 is the one converted), calibration influence on the result.
// ============================================================================
class Adc : public BusDevice32 {
public:
    static constexpr double kVrefVolts = 3.3;

    struct Traits {
        const char* name;
        int index;  // 1 = ADC1, 2 = ADC2
        int irq;    // NVIC IRQn (ADC1_2 = 18)
    };

    Adc(uint32_t base, Traits traits);

    void reset();

    // Board wiring: voltage present on ADC channel @ch (0..18). Called for the
    // channel selected by the sequencer at conversion time.
    void setChannelVoltageProvider(std::function<double(int ch)> fn) {
        channelVoltage_ = std::move(fn);
    }
    void setIrqCallback(std::function<void(int irq)> fn) {
        irqCallback_ = std::move(fn);
    }

    // ---- inspection (unit tests / board code) ----
    const Traits& traits() const { return traits_; }
    bool enabled() const { return (cr_ & kCrAden) != 0; }
    bool converting() const { return (cr_ & kCrAdstart) != 0; }
    uint32_t status() const { return isr_; }
    uint32_t dataRegister() const { return dr_; }
    uint32_t lastCode() const;
    int selectedChannel() const { return firstRankChannel(); }
    uint32_t resolutionBits() const;
    uint32_t maxCode() const;

private:
    // ---- register offsets (CMSIS ADC_TypeDef) ----
    static constexpr uint32_t R_ISR = 0x00, R_IER = 0x04, R_CR = 0x08,
                             R_CFGR = 0x0C, R_CFGR2 = 0x10, R_SMPR1 = 0x14,
                             R_SMPR2 = 0x18, R_TR1 = 0x20, R_TR2 = 0x24,
                             R_TR3 = 0x28, R_SQR1 = 0x30, R_SQR2 = 0x34,
                             R_SQR3 = 0x38, R_SQR4 = 0x3C, R_DR = 0x40,
                             R_JSQR = 0x4C, R_OFR1 = 0x60, R_JDR1 = 0x80,
                             R_AWD2CR = 0xA0, R_AWD3CR = 0xA4, R_DIFSEL = 0xB0,
                             R_CALFACT = 0xB4;
    // ---- bit definitions (verified against docs/stm32g431xx.h) ----
    static constexpr uint32_t kIsrAdrdy = 1u << 0, kIsrEoc = 1u << 2,
                             kIsrEos = 1u << 3, kIsrOvr = 1u << 4;
    static constexpr uint32_t kIerAdrdyie = 1u << 0, kIerEocie = 1u << 2,
                             kIerEosie = 1u << 3, kIerOvrie = 1u << 4;
    static constexpr uint32_t kCrAden = 1u << 0, kCrAddis = 1u << 1,
                             kCrAdstart = 1u << 2, kCrJadstart = 1u << 3,
                             kCrAdstp = 1u << 4, kCrAdvregen = 1u << 28,
                             kCrDeeppwd = 1u << 29, kCrAdcal = 1u << 31;
    static constexpr uint32_t kCfgrResMask = 0x3u << 3;   // CFGR[4:3]
    static constexpr uint32_t kCfgrOrmod = 1u << 12, kCfgrCont = 1u << 13,
                             kCfgrAlign = 1u << 15;
    static constexpr uint32_t kSqr1RankMask = 0x1Fu;      // L[3:0]
    static constexpr uint32_t kSqr1Sq1Shift = 6;          // SQ1[10:6]

    uint32_t readReg(uint32_t regOff) override;
    void writeReg(uint32_t regOff, uint32_t value) override;

    void startConversion();
    void convertOnce();
    int firstRankChannel() const {
        return int((sqr1_ >> kSqr1Sq1Shift) & 0x1Fu);
    }
    void fireIrq();

    Traits traits_;
    uint32_t isr_ = 0, ier_ = 0, cr_ = 0, cfgr_ = 0, cfgr2_ = 0;
    uint32_t smpr1_ = 0, smpr2_ = 0;
    uint32_t sqr1_ = 0, sqr2_ = 0, sqr3_ = 0, sqr4_ = 0;
    uint32_t dr_ = 0, jsqr_ = 0, calfact_ = 0, difsel_ = 0;
    uint32_t tr_[3] = {0, 0, 0};
    uint32_t ofr_[4] = {0, 0, 0, 0};
    uint32_t jdr_[4] = {0, 0, 0, 0};
    uint32_t awd2cr_ = 0, awd3cr_ = 0;

    std::function<double(int ch)> channelVoltage_;
    std::function<void(int irq)> irqCallback_;
};

// ---------------------------------------------------------------------------
// ADC12_COMMON register block (0x50000300). Only the clock configuration
// (CCR) and the common data register (CDR) live here; the firmware writes them
// during ADC setup, so they are accepted and stored.
// ---------------------------------------------------------------------------
class AdcCommonRegs : public BusDevice32 {
public:
    static constexpr uint32_t kBase = 0x50000300u;
    static constexpr uint32_t kSize = 0x40u;

    AdcCommonRegs() : BusDevice32(kBase) {}
    void reset() {
        for (auto& r : regs_) r = 0;
    }
    uint32_t ccr() const { return regs_[0x08 / 4]; }

protected:
    uint32_t readReg(uint32_t regOff) override {
        return (regOff < kSize) ? regs_[regOff / 4] : 0;
    }
    void writeReg(uint32_t regOff, uint32_t value) override {
        if (regOff < kSize) regs_[regOff / 4] = value;
    }

private:
    uint32_t regs_[kSize / 4] = {};
};