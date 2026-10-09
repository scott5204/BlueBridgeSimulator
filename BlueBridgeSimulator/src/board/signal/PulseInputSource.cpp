#include "board/signal/PulseInputSource.h"

void PulseInputSource::restart(uint64_t nowCycle, uint32_t hclk) {
    // Phase 0: the source starts low and produces its first rising edge
    // immediately; the following edges are half a period apart (50 % duty).
    level_ = false;
    highAcc_ = lowAcc_ = 0;
    lastCycle_ = nowCycle;
    if (!enabled_ || hclk == 0) {
        restartPending_ = enabled_;  // wait for a usable clock
        nextEdge_ = kNever;
        return;
    }
    restartPending_ = false;
    nextEdge_ = nowCycle;
}

uint64_t PulseInputSource::stepCycles(uint64_t& acc, uint32_t weight,
                                      uint32_t hclk) {
    if (freqHz_ == 0 || weight == 0 || hclk == 0) return 0;
    acc += uint64_t(hclk) * uint64_t(weight);
    const uint64_t den = uint64_t(freqHz_) * 65536ull;
    const uint64_t d = acc / den;
    acc %= den;
    return d;
}

void PulseInputSource::advance(uint64_t endCycle, uint32_t hclk,
                               const EdgeFn& onEdge) {
    lastCycle_ = endCycle;

    if (!enabled_) {
        level_ = false;
        nextEdge_ = kNever;
        restartPending_ = false;
        return;
    }
    if (restartPending_) {
        if (hclk == 0) return;  // no time base yet (clock tree not up)
        level_ = false;
        highAcc_ = lowAcc_ = 0;
        nextEdge_ = endCycle;
        restartPending_ = false;
    }
    if (nextEdge_ == kNever || hclk == 0) return;

    while (nextEdge_ <= endCycle) {
        const uint64_t at = nextEdge_;
        level_ = !level_;
        if (onEdge) onEdge(level_, at);
        // 50 % duty: each half accumulates half a period
        uint64_t delta = stepCycles(level_ ? highAcc_ : lowAcc_, kDutyQ16, hclk);
        if (delta == 0) delta = 1;  // never stall on a degenerate configuration
        nextEdge_ = at + delta;
    }
}