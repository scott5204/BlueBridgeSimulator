#include "board/signal/DigitalSignalMonitor.h"

void DigitalSignalMonitor::reset() {
    level_ = false;
    edges_ = 0;
    lastEdgeCycle_ = 0;
    lastRiseCycle_ = prevRiseCycle_ = lastFallCycle_ = 0;
    hasRise_ = hasPrevRise_ = hasPeriod_ = false;
    periodCycles_ = highCycles_ = 0;
}

void DigitalSignalMonitor::onLevel(bool level, uint64_t cpuCycle) {
    if (edges_ > 0 && level == level_) return;  // defensive: only real edges

    // A gap much longer than the last period means the source stopped and
    // started again (timer stopped, AF reconfigured, ...): the pair of edges
    // around the gap is NOT a period, so the acquisition restarts. Without
    // this a single stray edge after a long silence would look like one huge
    // "period" and keep the monitor reporting a live waveform.
    if (hasPeriod_ && cpuCycle > lastEdgeCycle_ &&
        (cpuCycle - lastEdgeCycle_) > 4ull * periodCycles_) {
        // Drop the whole acquisition, including the pending rise: the next
        // edge must start a fresh period, not be paired with a stale one.
        hasRise_ = false;
        hasPrevRise_ = false;
        hasPeriod_ = false;
        highCycles_ = 0;
    }

    edges_++;
    level_ = level;
    lastEdgeCycle_ = cpuCycle;

    if (level) {  // rising edge: close the previous period
        if (hasRise_) {
            prevRiseCycle_ = lastRiseCycle_;
            hasPrevRise_ = true;
        }
        lastRiseCycle_ = cpuCycle;
        hasRise_ = true;
        if (hasPrevRise_ && lastRiseCycle_ > prevRiseCycle_) {
            periodCycles_ = lastRiseCycle_ - prevRiseCycle_;
            hasPeriod_ = true;
        }
    } else if (hasRise_ && cpuCycle >= lastRiseCycle_) {
        highCycles_ = cpuCycle - lastRiseCycle_;  // high time of this period
        lastFallCycle_ = cpuCycle;
    }
}

DigitalSignalMonitor::Stats DigitalSignalMonitor::measure(uint64_t nowCycle,
                                                          uint32_t hclk) const {
    Stats s;
    s.level = level_;
    s.edges = edges_;
    s.lastRiseCycle = lastRiseCycle_;
    s.lastFallCycle = lastFallCycle_;
    s.periodCycles = hasPeriod_ ? periodCycles_ : 0;
    s.highCycles = highCycles_;
    if (!hasPeriod_ || hclk == 0) return s;

    s.frequencyHz = double(hclk) / double(periodCycles_);
    s.duty = double(highCycles_) / double(periodCycles_);
    // Silence for four periods means the source stopped (CEN=0, AF wrong,
    // counter reconfigured, ...): report "no waveform" instead of stale data
    // (the GUI shows Frequency 0 / Duty -- / Status INACTIVE).
    const uint64_t silence =
        (nowCycle >= lastEdgeCycle_) ? (nowCycle - lastEdgeCycle_) : 0;
    s.active = silence <= 4ull * periodCycles_;
    if (!s.active) {
        s.frequencyHz = 0.0;
        s.duty = 0.0;
    }
    return s;
}