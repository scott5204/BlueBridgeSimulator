#include "board/analog/AnalogSource.h"

AnalogSource::AnalogSource() : voltage_(0.0), minV_(0.0), maxV_(3.3) {}

void AnalogSource::setRange(double lo, double hi) {
    minV_ = lo;
    maxV_ = (hi > lo) ? hi : lo;
    setVoltage(voltage_);  // re-clamp to the new range
}

void AnalogSource::setVoltage(double volts) {
    voltage_ = (volts < minV_) ? minV_ : (volts > maxV_) ? maxV_ : volts;
}

double AnalogSource::voltage() const { return voltage_; }

double AnalogSource::minVoltage() const { return minV_; }

double AnalogSource::maxVoltage() const { return maxV_; }