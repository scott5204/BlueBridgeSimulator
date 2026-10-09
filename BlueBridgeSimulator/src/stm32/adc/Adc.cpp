#include "stm32/adc/Adc.h"

#include <cmath>

Adc::Adc(uint32_t base, Traits traits) : BusDevice32(base), traits_(traits) {
    reset();
}

void Adc::reset() {
    isr_ = ier_ = cr_ = cfgr_ = cfgr2_ = 0;
    smpr1_ = smpr2_ = 0;
    sqr1_ = sqr2_ = sqr3_ = sqr4_ = 0;
    dr_ = jsqr_ = calfact_ = difsel_ = 0;
    for (auto& r : tr_) r = 0;
    for (auto& r : ofr_) r = 0;
    for (auto& r : jdr_) r = 0;
    awd2cr_ = awd3cr_ = 0;
}

// ---------------------------------------------------------------------------
// resolution
// ---------------------------------------------------------------------------
uint32_t Adc::resolutionBits() const {
    switch ((cfgr_ & kCfgrResMask) >> 3) {
    case 0: return 12;  // RES = 00
    case 1: return 10;  // RES = 01
    case 2: return 8;   // RES = 10
    default: return 6;  // RES = 11
    }
}

uint32_t Adc::maxCode() const { return (1u << resolutionBits()) - 1u; }

uint32_t Adc::lastCode() const {
    // undo the alignment so tests can compare codes regardless of ALIGN
    if (cfgr_ & kCfgrAlign) return dr_ >> (16 - resolutionBits());
    return dr_ & maxCode();
}

// ---------------------------------------------------------------------------
// registers
// ---------------------------------------------------------------------------
uint32_t Adc::readReg(uint32_t regOff) {
    switch (regOff) {
    case R_ISR: return isr_;
    case R_IER: return ier_;
    case R_CR: return cr_;
    case R_CFGR: return cfgr_;
    case R_CFGR2: return cfgr2_;
    case R_SMPR1: return smpr1_;
    case R_SMPR2: return smpr2_;
    case R_TR1: case R_TR1 + 4: case R_TR1 + 8:
        return tr_[(regOff - R_TR1) / 4];
    case R_SQR1: return sqr1_;
    case R_SQR2: return sqr2_;
    case R_SQR3: return sqr3_;
    case R_SQR4: return sqr4_;
    case R_DR: {
        // RM0440: reading DR clears EOC and EOS; with continuous conversion
        // the next conversion starts immediately (so the polling firmware
        // always sees a fresh EOC).
        const uint32_t v = dr_;
        isr_ &= ~(kIsrEoc | kIsrEos);
        if ((cfgr_ & kCfgrCont) && (cr_ & kCrAden)) convertOnce();
        return v;
    }
    case R_JSQR: return jsqr_;
    case R_OFR1: case R_OFR1 + 4: case R_OFR1 + 8: case R_OFR1 + 12:
        return ofr_[(regOff - R_OFR1) / 4];
    case R_JDR1: case R_JDR1 + 4: case R_JDR1 + 8: case R_JDR1 + 12:
        return jdr_[(regOff - R_JDR1) / 4];
    case R_AWD2CR: return awd2cr_;
    case R_AWD3CR: return awd3cr_;
    case R_DIFSEL: return difsel_;
    case R_CALFACT: return calfact_;
    default: return 0;
    }
}

void Adc::writeReg(uint32_t regOff, uint32_t value) {
    switch (regOff) {
    case R_ISR:
        // OVR is cleared by writing 1 (the other flags are hardware owned)
        isr_ &= ~(value & kIsrOvr);
        break;
    case R_IER: ier_ = value; break;
    case R_CR: {
        // ---- calibration (HAL_ADCEx_Calibration_Start) ----
        // On real silicon ADCAL pulls ADEN low and self-clears when done.
        if (value & kCrAdcal) {
            cr_ &= ~(kCrAden | kCrAdstart);
            isr_ &= ~(kIsrAdrdy | kIsrEoc | kIsrEos);
            calfact_ = 0;
            cr_ = (cr_ & ~kCrAdcal) | (value & (kCrAdvregen | kCrDeeppwd));
            break;
        }
        // ---- enable / disable ----
        if (value & kCrAddis) {
            cr_ &= ~(kCrAden | kCrAdstart);
            isr_ &= ~(kIsrAdrdy | kIsrEoc | kIsrEos);
            cr_ &= ~kCrAddis;
            break;
        }
        if ((value & kCrAden) && !(cr_ & kCrAden)) {
            cr_ |= kCrAden;
            isr_ |= kIsrAdrdy;  // ready as soon as it is enabled (model)
        }
        // ---- start / stop a regular conversion ----
        if (value & kCrAdstart) {
            if (cr_ & kCrAden) startConversion();
        }
        if (value & kCrAdstp) {
            cr_ &= ~(kCrAdstart | kCrJadstart);
        }
        cr_ = (cr_ & ~kCrAdvregen) | (value & kCrAdvregen);
        cr_ = (cr_ & ~kCrDeeppwd) | (value & kCrDeeppwd);
        break;
    }
    case R_CFGR: cfgr_ = value; break;
    case R_CFGR2: cfgr2_ = value; break;
    case R_SMPR1: smpr1_ = value; break;
    case R_SMPR2: smpr2_ = value; break;
    case R_TR1: case R_TR1 + 4: case R_TR1 + 8:
        tr_[(regOff - R_TR1) / 4] = value;
        break;
    case R_SQR1: sqr1_ = value; break;
    case R_SQR2: sqr2_ = value; break;
    case R_SQR3: sqr3_ = value; break;
    case R_SQR4: sqr4_ = value; break;
    case R_JSQR: jsqr_ = value; break;
    case R_OFR1: case R_OFR1 + 4: case R_OFR1 + 8: case R_OFR1 + 12:
        ofr_[(regOff - R_OFR1) / 4] = value;
        break;
    case R_JDR1: case R_JDR1 + 4: case R_JDR1 + 8: case R_JDR1 + 12:
        jdr_[(regOff - R_JDR1) / 4] = value;
        break;
    case R_AWD2CR: awd2cr_ = value; break;
    case R_AWD3CR: awd3cr_ = value; break;
    case R_DIFSEL: difsel_ = value; break;
    case R_CALFACT: calfact_ = value & 0x7Fu; break;
    case R_DR: break;  // read-only
    default: break;
    }
}

// ---------------------------------------------------------------------------
// conversion
// ---------------------------------------------------------------------------
void Adc::fireIrq() {
    if (traits_.irq >= 0 && irqCallback_) irqCallback_(traits_.irq);
}

void Adc::convertOnce() {
    const int ch = firstRankChannel();
    double v = channelVoltage_ ? channelVoltage_(ch) : 0.0;
    if (!(v > 0.0)) v = 0.0;                 // also catches NaN
    if (v > kVrefVolts) v = kVrefVolts;
    const uint32_t maxC = maxCode();
    uint32_t code = uint32_t(v / kVrefVolts * double(maxC) + 0.5);
    if (code > maxC) code = maxC;

    const uint32_t bits = resolutionBits();
    dr_ = (cfgr_ & kCfgrAlign) ? (code << (16u - bits)) : code;

    isr_ |= kIsrEoc | kIsrEos;
    if (ier_ & (kIerEocie | kIerEosie)) fireIrq();
}

void Adc::startConversion() {
    if (!(cr_ & kCrAden)) return;
    if (isr_ & kIsrEoc) isr_ |= kIsrOvr;  // previous result not read yet
    cr_ |= kCrAdstart;
    convertOnce();
    // single conversion mode: ADSTART is cleared by hardware when the
    // sequence is complete (RM0440); CONT keeps it running
    if (!(cfgr_ & kCfgrCont)) cr_ &= ~kCrAdstart;
}