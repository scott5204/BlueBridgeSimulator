#pragma once

// ============================================================================
// Board profile: the user-facing input ranges of a board, kept in ONE place so
// they are not scattered through the GUI (project rule: the GUI reads them
// from the board model, it never hard-codes hardware limits).
//
// The training ranges follow the blue-bridge competition practice: the PA15
// pulse input covers 400 Hz .. 20 kHz (frequency measurement tasks) and both
// potentiometers cover 0 .. 3.3 V (the board's VDD reference).
//
// The pulse SOURCE itself is not limited to this range (PulseInputSource goes
// up to 100 kHz); only the GUI knobs are.
// ============================================================================
struct BoardProfile {
    double pulseMinHz = 400.0;
    double pulseMaxHz = 20000.0;

    double analogMinVoltage = 0.0;
    double analogMaxVoltage = 3.3;
};

// CT117E-M4 defaults (0 .. 3.3 V potentiometers, 400 Hz .. 20 kHz pulse input)
inline BoardProfile ct117eM4Profile() { return BoardProfile{}; }