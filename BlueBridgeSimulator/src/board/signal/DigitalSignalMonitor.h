#pragma once

#include <cstdint>

// ============================================================================
// Digital signal monitor (board-level "oscilloscope" for a pin waveform).
//
// Connected to a real board pin through GpioPort::addLevelObserver(), so it
// measures the FINAL pin waveform -- after the timer channel, the alternate
// function routing, the GPIO mode and the polarity handling:
//
//   firmware -> TIM -> channel -> GPIO AF -> PA7 -> this monitor -> GUI
//
// A firmware that forgot MODER=AF, selected the wrong channel, disabled CCxE
// or never started the counter simply produces no edges here, which is exactly
// what the GUI must report (Frequency 0 / INACTIVE).
//
// Edge timestamps are the exact virtual (CPU) cycle of the change, so period,
// high time, frequency and duty are computed from real edge times -- never
// from PSC/ARR/CCR register values.
// ============================================================================
class DigitalSignalMonitor {
public:
    struct Stats {
        bool active = false;        // toggling right now (see measure())
        bool level = false;         // current pin level
        uint64_t edges = 0;         // total level changes observed
        uint64_t lastRiseCycle = 0;      // timestamp of the last rising edge
        uint64_t lastFallCycle = 0;      // timestamp of the last falling edge
        uint64_t periodCycles = 0;       // last rising edge to rising edge
        uint64_t highCycles = 0;         // last rising edge to falling edge
        double frequencyHz = 0.0;        // 0 = unknown / not toggling
        double duty = 0.0;               // 0..1
    };

    void reset();

    // Pin level change with its exact virtual (CPU) cycle.
    void onLevel(bool level, uint64_t cpuCycle);

    // Evaluate at the current virtual time. The waveform counts as active only
    // while edges keep arriving (4 periods of silence = stopped).
    Stats measure(uint64_t nowCycle, uint32_t hclk) const;

private:
    bool level_ = false;
    uint64_t edges_ = 0;
    uint64_t lastEdgeCycle_ = 0;
    uint64_t lastRiseCycle_ = 0;
    uint64_t prevRiseCycle_ = 0;
    uint64_t lastFallCycle_ = 0;
    bool hasRise_ = false;
    bool hasPrevRise_ = false;
    bool hasPeriod_ = false;
    uint64_t periodCycles_ = 0;
    uint64_t highCycles_ = 0;
};