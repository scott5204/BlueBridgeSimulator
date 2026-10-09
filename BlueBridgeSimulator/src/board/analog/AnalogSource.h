#pragma once

// ============================================================================
// Board-level analog source: the "potentiometer" behind R37 / R38.
//
// Like the pulse input source, the analog front end is NOT modelled (no
// resistor divider, no supply impedance, no noise): the GUI sets a voltage and
// this source holds it. That voltage is what the MCU pin sees, so the full
// path stays real:
//
//   GUI -> AnalogSource -> MCU analog pin -> ADC channel -> ADC conversion
//       -> ADC_DR -> firmware
//
// The value is clamped to the board range (0 .. 3.3 V on CT117E-M4).
// ============================================================================
class AnalogSource {
public:
    AnalogSource();

    void setRange(double lo, double hi);
    void setVoltage(double volts);
    double voltage() const;
    double minVoltage() const;
    double maxVoltage() const;

private:
    double voltage_;
    double minV_;
    double maxV_;
};