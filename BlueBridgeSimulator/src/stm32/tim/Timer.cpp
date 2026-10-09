#include "stm32/tim/Timer.h"

#include <algorithm>
#include <cstdio>

namespace {
constexpr uint32_t kEgrCc2g = 1u << 2;
constexpr uint32_t kEgrCc3g = 1u << 3;
constexpr uint32_t kEgrCc4g = 1u << 4;

// DIER interrupt enable bits (channel capture/compare) follow the SR layout
constexpr uint32_t kCcIeBit(int ch) { return 1u << ch; }  // UIE=bit0, CC1IE=1...
constexpr uint32_t kCcIfBit(int ch) { return 1u << ch; }  // UIF=bit0, CC1IF=1...

std::string hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%04X", unsigned(v));
    return buf;
}
}  // namespace

Timer::Timer(uint32_t base, Traits traits)
    : BusDevice32(base), traits_(traits) {
    reset();
}

void Timer::reset() {
    cr1_ = cr2_ = smcr_ = dier_ = sr_ = 0;
    ccmr1_ = ccmr2_ = ccer_ = bdtr_ = 0;
    cnt_ = 0;
    pscPre_ = pscAct_ = 0;
    arrPre_ = arrAct_ = counterMask();
    for (int i = 0; i < 4; i++) {
        ccrPre_[i] = ccrAct_[i] = ccrCaptured_[i] = 0;
        outDriven_[i] = outLevel_[i] = inLevel_[i] = false;
    }
    rcr_ = dcr_ = dmar_ = af1_ = af2_ = tisel_ = 0;
    rep_ = 0;
    clockAcc_ = prescAcc_ = tickCpuAcc_ = cpuCursor_ = 0;
    pscUsed_ = 0;
    warnedDownCount_ = warnedCenterAlign_ = warnedAdvanced_ = false;
}

// ---------------------------------------------------------------------------
// register file
// ---------------------------------------------------------------------------
uint32_t Timer::ccmrByte(uint32_t ccmr, int chInPair) {
    return (ccmr >> (chInPair * 8)) & 0xFFu;
}

bool Timer::channelIsOutput(uint32_t ccmr, int chInPair) {
    return (ccmrByte(ccmr, chInPair) & 0x3u) == 0;  // CCxS = 00
}

uint32_t Timer::channelMode(uint32_t ccmr, int chInPair) {
    return (ccmrByte(ccmr, chInPair) >> 4) & kOcModeMask;  // OCxM
}

bool Timer::channelEnabled(int ch) const {
    return (ccer_ & (1u << (ch * 4))) != 0;  // CCxE
}

bool Timer::channelPolarity(int ch) const {
    return (ccer_ & (1u << (ch * 4 + 1))) != 0;  // CCxP
}

bool Timer::channelIsCapture(int ch) const {
    if (ch >= traits_.channels) return false;
    return !channelIsOutput(ccmrFor(ch), chInPair(ch));
}

bool Timer::channelOutputDriven(int ch) const {
    if (ch >= traits_.channels) return false;
    const uint32_t ccmr = ccmrFor(ch);
    if (!channelIsOutput(ccmr, chInPair(ch))) return false;
    if (!channelEnabled(ch)) return false;
    const uint32_t mode = channelMode(ccmr, chInPair(ch));
    if (mode != kOcModePwm1 && mode != kOcModePwm2) return false;
    if (traits_.advanced && !(bdtr_ & (1u << 15))) return false;  // MOE
    return true;
}

bool Timer::channelOutputActiveLevel(int ch) const {
    const uint32_t ccmr = ccmrFor(ch);
    const uint32_t mode = channelMode(ccmr, chInPair(ch));
    const uint32_t ccr = ccrAct_[ch];
    // OCxREF: PWM1 = high while CNT < CCR, PWM2 = the opposite
    bool ref = (mode == kOcModePwm1) ? (cnt_ < ccr) : (cnt_ >= ccr);
    return channelPolarity(ch) ? !ref : ref;
}

bool Timer::captureEdgeMatches(int ch, bool level) const {
    const bool pol = channelPolarity(ch);   // CCxP: 0 = rising, 1 = falling
    const bool both = (ccer_ & (1u << (ch * 4 + 3))) != 0;  // CCxNP
    if (level) return !pol;                 // rising edge
    return pol || both;                     // falling edge (always with CCxP=1)
}

bool Timer::outputDriven(int ch) const { return outDriven_[ch]; }
bool Timer::outputLevel(int ch) const { return outLevel_[ch]; }

uint32_t Timer::readReg(uint32_t regOff) {
    switch (regOff) {
    case R_CR1: return cr1_;
    case R_CR2: return cr2_;
    case R_SMCR: return smcr_;
    case R_DIER: return dier_;
    case R_SR: return sr_;
    case R_EGR: return 0;  // write-only
    case R_CCMR1: return ccmr1_;
    case R_CCMR2: return ccmr2_;
    case R_CCER: return ccer_;
    case R_CNT: return cnt_;
    case R_PSC: return pscPre_;
    case R_ARR: return arrPre_;
    case R_RCR: return traits_.advanced ? (rep_ & 0xFFu) : 0;
    case R_CCR1: case R_CCR1 + 4: case R_CCR1 + 8: case R_CCR1 + 12: {
        const int ch = int((regOff - R_CCR1) / 4);
        if (ch >= 4) break;
        // capture channels report the latched value, output channels the
        // preload value (RM0440 TIMx_CCRx behaviour)
        return channelIsCapture(ch) ? ccrCaptured_[ch] : ccrPre_[ch];
    }
    case R_BDTR: return traits_.advanced ? bdtr_ : 0;
    case R_DCR: return dcr_;
    case R_DMAR: return dmar_;
    case R_AF1: return af1_;
    case R_AF2: return af2_;
    case R_TISEL: return tisel_;
    default: return 0;
    }
    return 0;
}

void Timer::writeReg(uint32_t regOff, uint32_t value) {
    switch (regOff) {
    case R_CR1: {
        if ((value & kCr1Cms) && !warnedCenterAlign_) {
            warnedCenterAlign_ = true;
            traceEvent(std::string(traits_.name) +
                       ": center-aligned mode is not modelled (up-counting "
                       "only)");
        }
        if ((value & kCr1Dir) && !warnedDownCount_) {
            warnedDownCount_ = true;
            traceEvent(std::string(traits_.name) +
                       ": down-counting is not modelled (up-counting only)");
        }
        cr1_ = value;
        if (traceRegs_)
            traceEvent(std::string("[") + traits_.name + "] CR1=" +
                       hex(value) + ((value & kCr1Cen) ? " CEN=1" : " CEN=0"));
        break;
    }
    case R_CR2: cr2_ = value; break;
    case R_SMCR: smcr_ = value; break;  // slave mode not modelled
    case R_DIER:
        dier_ = value & 0xFFFFu;
        // enabling an interrupt whose flag is already set must request the IRQ
        if ((dier_ & kUie) && (sr_ & kUif)) fireIrq();
        for (int ch = 0; ch < 4; ch++)
            if ((dier_ & kCcIeBit(ch + 1)) && (sr_ & kCcIfBit(ch + 1)))
                fireIrq();
        break;
    case R_SR:
        // STM32 TIMx_SR is cleared by writing 0 (RM0440 / HAL writes ~flag)
        sr_ &= value;
        break;
    case R_EGR: {
        const bool ug = (value & kEgUg) != 0;
        for (int ch = 0; ch < 4; ch++) {
            if (!(value & (1u << (ch + 1)))) continue;  // CCxG
            if (channelIsCapture(ch)) {
                ccrCaptured_[ch] = cnt_;
                sr_ |= kCcIfBit(ch + 1);
                if (dier_ & kCcIeBit(ch + 1)) fireIrq();
            } else {
                sr_ |= kCcIfBit(ch + 1);
                if (dier_ & kCcIeBit(ch + 1)) fireIrq();
            }
        }
        if (ug) {
            cnt_ = 0;
            // the prescaler buffer is reloaded and the counter restarted
            pscAct_ = pscPre_;
            pscUsed_ = pscAct_;
            prescAcc_ = 0;
            if (cr1_ & kCr1Arpe) arrAct_ = arrPre_;
            for (int ch = 0; ch < 4; ch++)
                if (ccmrByte(ccmrFor(ch), chInPair(ch)) & 0x8u)  // OCxPE
                    ccrAct_[ch] = ccrPre_[ch];
            onUpdateEvent(/*fromUg=*/true);
            updateAllChannelOutputs(cpuCursor_);
            if (traceEvents_)
                traceEvent(std::string("[") + traits_.name + "] UG (update "
                           "generation)");
        }
        break;
    }
    case R_CCMR1: ccmr1_ = value; updateAllChannelOutputs(cpuCursor_); break;
    case R_CCMR2: ccmr2_ = value; updateAllChannelOutputs(cpuCursor_); break;
    case R_CCER: ccer_ = value; updateAllChannelOutputs(cpuCursor_); break;
    case R_CNT: cnt_ = value & counterMask(); break;
    case R_PSC:
        pscPre_ = value & 0xFFFFu;
        if (!(cr1_ & kCr1Cen)) pscUsed_ = pscPre_;  // takes effect at the next UG
        break;
    case R_ARR:
        arrPre_ = value & counterMask();
        if (!(cr1_ & kCr1Arpe)) arrAct_ = arrPre_;  // ARPE=0: immediate
        break;
    case R_RCR: rcr_ = value & 0xFFu; break;
    case R_CCR1: case R_CCR1 + 4: case R_CCR1 + 8: case R_CCR1 + 12: {
        const int ch = int((regOff - R_CCR1) / 4);
        if (ch >= 4) break;
        ccrPre_[ch] = value & counterMask();
        const bool preload =
            (ccmrByte(ccmrFor(ch), chInPair(ch)) & 0x8u) != 0;  // OCxPE
        if (!preload) ccrAct_[ch] = ccrPre_[ch];
        if (channelIsCapture(ch)) ccrCaptured_[ch] = ccrPre_[ch];
        publishChannel(ch, /*force=*/true, cpuCursor_);
        if (traceEvents_)
            traceEvent(std::string("[") + traits_.name + "] CCR" +
                       std::to_string(ch + 1) + "=" + hex(ccrPre_[ch]));
        break;
    }
    case R_BDTR:
        bdtr_ = value;
        if (traits_.advanced && (value & (1u << 15)) && traceEvents_)
            traceEvent(std::string("[") + traits_.name + "] MOE=1");
        updateAllChannelOutputs(cpuCursor_);
        break;
    case R_DCR: dcr_ = value; break;
    case R_DMAR: dmar_ = value; break;
    case R_AF1: af1_ = value; break;
    case R_AF2: af2_ = value; break;
    case R_TISEL: tisel_ = value; break;
    default: break;  // reserved: stored nowhere, no side effects
    }
}

// ---------------------------------------------------------------------------
// update event
// ---------------------------------------------------------------------------
void Timer::onUpdateEvent(bool fromUg) {
    // prescaler buffer reload, autoreload preload, repetition counter
    pscAct_ = pscPre_;
    pscUsed_ = pscAct_;
    prescAcc_ = 0;
    if (cr1_ & kCr1Arpe) arrAct_ = arrPre_;
    for (int ch = 0; ch < 4; ch++)
        if (ccmrByte(ccmrFor(ch), chInPair(ch)) & 0x8u)  // OCxPE
            ccrAct_[ch] = ccrPre_[ch];
    rep_ = rcr_;

    if (!(cr1_ & kCr1Udis)) {
        // overflow always sets UIF; UG only when URS = 0 (RM0440 TIMx_SR)
        if (!fromUg || !(cr1_ & kCr1Urs)) {
            sr_ |= kUif;
            if (dier_ & kUie) fireIrq();
            if (traceEvents_)
                traceEvent(std::string("[") + traits_.name + "] UPDATE (UIF)");
        }
    }
}

void Timer::fireIrq() {
    if (traits_.irq >= 0 && irqCallback_) irqCallback_(traits_.irq);
}

void Timer::traceEvent(const std::string& s) const {
    if (logger_) logger_(s);
}

// ---------------------------------------------------------------------------
// channel outputs
// ---------------------------------------------------------------------------
void Timer::publishChannel(int ch, bool force, uint64_t cpuCycle) {
    if (ch < 0 || ch >= 4) return;
    const bool driven = channelOutputDriven(ch);
    const bool level = driven ? channelOutputActiveLevel(ch) : false;
    if (!force && driven == outDriven_[ch] && level == outLevel_[ch]) return;
    const bool wasDriven = outDriven_[ch];
    const bool wasLevel = outLevel_[ch];
    outDriven_[ch] = driven;
    outLevel_[ch] = level;
    if (channelOut_ &&
        (force || driven != wasDriven || level != wasLevel))
        channelOut_(ch, driven, level, cpuCycle);
}

void Timer::updateAllChannelOutputs(uint64_t cpuCycle) {
    for (int ch = 0; ch < traits_.channels; ch++)
        publishChannel(ch, /*force=*/false, cpuCycle);
}

// ---------------------------------------------------------------------------
// channel inputs (capture)
// ---------------------------------------------------------------------------
void Timer::refreshChannelOutputs() {
    for (int ch = 0; ch < traits_.channels; ch++)
        publishChannel(ch, /*force=*/true, cpuCursor_);
}

void Timer::onChannelPinLevel(int ch, bool level, uint64_t cpuCycle) {
    if (ch < 0 || ch >= 4) return;
    inLevel_[ch] = level;
    if (!(cr1_ & kCr1Cen)) return;
    if (!channelIsCapture(ch)) return;
    if (!channelEnabled(ch)) return;
    if (!captureEdgeMatches(ch, level)) return;

    ccrCaptured_[ch] = cnt_;
    ccrPre_[ch] = cnt_;  // what the firmware reads back
    sr_ |= kCcIfBit(ch + 1);
    if (dier_ & kCcIeBit(ch + 1)) fireIrq();
    if (traceEvents_)
        traceEvent(std::string("[") + traits_.name + "] CAPTURE CH" +
                   std::to_string(ch + 1) + "=" + hex(cnt_) +
                   (level ? " (rising)" : " (falling)"));
}

// ---------------------------------------------------------------------------
// time base
// ---------------------------------------------------------------------------
void Timer::advance(uint64_t endCpuCycle, uint64_t cpuCycles, uint32_t hclk,
                    uint32_t timHz) {
    cpuCursor_ = (endCpuCycle >= cpuCycles) ? (endCpuCycle - cpuCycles) : 0;

    if (!(cr1_ & kCr1Cen) || hclk == 0 || timHz == 0) {
        // the counter is stopped: no time debt accumulates
        clockAcc_ = prescAcc_ = tickCpuAcc_ = 0;
        cpuCursor_ = endCpuCycle;
        return;
    }
    if (cpuCycles == 0) return;

    clockAcc_ += cpuCycles * uint64_t(timHz);
    const uint64_t timerClocks = clockAcc_ / hclk;
    clockAcc_ %= hclk;
    if (timerClocks == 0) return;

    prescAcc_ += timerClocks;
    const uint64_t div = uint64_t(pscUsed_) + 1;
    uint64_t ticks = prescAcc_ / div;
    prescAcc_ %= div;
    if (ticks == 0) return;

    // Each pass consumes at least one timer tick, so this bound can never be
    // reached with a sane input; it exists so a corrupt value cannot spin here
    // forever (see Stm32G431::advanceTime / PROJECT_HANDOFF 7.6).
    uint64_t guard = ticks + 1;
    while (ticks > 0 && guard-- > 0) {
        // Boundaries ahead: counter wrap (update event) and CCR matches.
        //
        // The firmware may legally lower ARR while the counter is running
        // (frequency changes) -- CNT can then be ABOVE the new ARR. On real
        // silicon the counter keeps counting and simply wraps at the counter
        // size (0xFFFF / 0xFFFFFFFF), so the distance to the wrap must be
        // measured against the counter mask in that case. (Without this the
        // subtraction wraps around and the timer stops producing edges.)
        const uint64_t toWrap = (cnt_ <= arrAct_)
                                    ? uint64_t(arrAct_) - cnt_ + 1
                                    : uint64_t(counterMask()) - cnt_ + 1;
        uint64_t toCmp = ~0ull;
        for (int ch = 0; ch < traits_.channels; ch++) {
            if (!channelOutputDriven(ch)) continue;
            const uint32_t c = ccrAct_[ch];
            uint64_t d;
            if (cnt_ < c) {
                d = uint64_t(c - cnt_);
            } else if (cnt_ <= arrAct_) {
                d = (uint64_t(arrAct_) - cnt_ + 1) + c;  // cross ARR, then to c
            } else {
                d = (uint64_t(counterMask()) - cnt_ + 1) + c;
            }
            toCmp = std::min(toCmp, d);
        }
        const uint64_t boundary = std::min(toWrap, toCmp);
        const uint64_t take = std::min(ticks, boundary);

        // advance the CPU-cycle cursor and the counter
        tickCpuAcc_ += take * uint64_t(hclk) * div;
        cpuCursor_ += tickCpuAcc_ / timHz;
        tickCpuAcc_ %= timHz;
        cnt_ += uint32_t(take);
        ticks -= take;

        if (take == boundary) {
            if (take == toWrap) {
                cnt_ = 0;
                onUpdateEvent(/*fromUg=*/false);
            }
            updateAllChannelOutputs(cpuCursor_);
        }
    }
}

uint64_t Timer::cpuCyclesToNextIrq(uint32_t hclk, uint32_t timHz) const {
    if (!(cr1_ & kCr1Cen) || hclk == 0 || timHz == 0) return ~0ull;
    if (!(dier_ & kUie)) return ~0ull;  // only interrupts need timely service
    // same "ARR lowered below CNT" case as in advance(): the counter then wraps
    // at the counter size, not at ARR
    const uint64_t toWrap = (cnt_ <= arrAct_)
                                ? uint64_t(arrAct_) - cnt_ + 1
                                : uint64_t(counterMask()) - cnt_ + 1;
    const uint64_t div = uint64_t(pscUsed_) + 1;
    const uint64_t timerClocks = toWrap * div;
    // ceil to CPU cycles
    const uint64_t num = timerClocks * uint64_t(hclk);
    return (num + timHz - 1) / timHz;
}