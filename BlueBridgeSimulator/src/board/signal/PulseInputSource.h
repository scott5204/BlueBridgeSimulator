#pragma once

#include <cstdint>
#include <functional>

// ============================================================================
// Board-level pulse input source (the "555 / external generator" substitute).
//
// The real CT117E-M4 board feeds PA15 (and PB4) from an XL555 oscillator with
// a potentiometer. This simulator deliberately does NOT model the analog
// circuit: the GUI just sets a frequency and this class turns it into a REAL
// digital waveform on the pin:
//
//   GUI -> PulseInputSource -> PA15 pin -> GPIO AF -> TIMx input capture
//
// Duty is fixed at 50 % (competition use is frequency / period measurement).
// Timing is derived from the simulator's VIRTUAL time (CPU cycles) with
// integer phase accumulators, so the pulse stays synchronous with SysTick,
// the timers and the PWM output at any simulation speed (1x / 10x / Max).
// There is no wall clock, no QTimer and no Sleep anywhere in this class.
// ============================================================================
class PulseInputSource {
public:
    static constexpr uint32_t kDefaultFreqHz = 1000;
    static constexpr uint32_t kMinFreqHz = 1;
    // well above the training range (400 Hz .. 20 kHz): the GUI limits the
    // range through BoardProfile, the hardware model does not
    static constexpr uint32_t kMaxFreqHz = 100000;
    static constexpr uint32_t kDutyQ16 = 32768;  // fixed 50 %

    using EdgeFn = std::function<void(bool level, uint64_t cpuCycle)>;

    PulseInputSource() = default;

    // ---- configuration (any change restarts the phase at @nowCycle) ----
    void setEnabled(bool enabled, uint64_t nowCycle, uint32_t hclk) {
        if (enabled == enabled_) return;
        enabled_ = enabled;
        restart(nowCycle, hclk);
    }
    void setFrequency(double hz, uint64_t nowCycle, uint32_t hclk) {
        setFrequencyHz(uint32_t(hz < 0.0 ? 0.0 : hz + 0.5), nowCycle, hclk);
    }
    void setFrequencyHz(uint32_t freqHz, uint64_t nowCycle, uint32_t hclk) {
        const uint32_t f = clampFreq(freqHz, hclk);
        if (f == freqHz_) return;
        freqHz_ = f;
        if (enabled_) restart(nowCycle, hclk);
    }
    // Restart the waveform phase without touching the configuration.
    void restartPhase(uint64_t nowCycle, uint32_t hclk) {
        restart(nowCycle, hclk);
    }

    bool enabled() const { return enabled_; }
    // The pin is driven (to level()) while the source is enabled.
    bool drivesPin() const { return enabled_; }
    double frequency() const { return double(freqHz_); }
    uint32_t frequencyHz() const { return freqHz_; }
    bool level() const { return level_; }

    // HCLK cycles from the last advance() to the next edge (~0ull = none).
    uint64_t cyclesToNextEdge() const {
        if (!enabled_) return ~0ull;
        if (restartPending_ || nextEdge_ == kNever) return 0;
        return (nextEdge_ > lastCycle_) ? (nextEdge_ - lastCycle_) : 0;
    }

    // Apply every edge scheduled at or before @endCycle, in time order.
    void advance(uint64_t endCycle, uint32_t hclk, const EdgeFn& onEdge);

    static uint32_t clampFreq(uint32_t freqHz, uint32_t hclk) {
        uint32_t maxF = kMaxFreqHz;
        if (hclk >= 8) {
            const uint32_t hclkLimit = hclk / 8;  // >= 8 cycles per period
            if (hclkLimit < maxF) maxF = hclkLimit;
        }
        if (maxF < kMinFreqHz) maxF = kMinFreqHz;
        if (freqHz < kMinFreqHz) return kMinFreqHz;
        if (freqHz > maxF) return maxF;
        return freqHz;
    }

private:
    static constexpr uint64_t kNever = ~0ull;

    void restart(uint64_t nowCycle, uint32_t hclk);
    // One edge interval: accumulate hclk * weight / (freq * 65536) cycles.
    uint64_t stepCycles(uint64_t& acc, uint32_t weight, uint32_t hclk);

    bool enabled_ = false;
    uint32_t freqHz_ = kDefaultFreqHz;
    bool level_ = false;

    bool restartPending_ = false;
    uint64_t nextEdge_ = kNever;  // absolute CPU cycle of the next edge
    uint64_t lastCycle_ = 0;      // time of the last advance()

    // integer phase accumulators (remainders): never drift
    uint64_t highAcc_ = 0;
    uint64_t lowAcc_ = 0;
};