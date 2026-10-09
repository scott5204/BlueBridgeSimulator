#pragma once

#include <atomic>
#include <cstdint>

#include "board/BoardProfile.h"
#include "board/analog/AnalogSource.h"
#include "board/lcd/LcdController.h"
#include "board/serial/VirtualSerialPeer.h"
#include "board/signal/DigitalSignalMonitor.h"
#include "board/signal/PulseInputSource.h"
#include "stm32/Stm32G431.h"
#include "stm32/gpio/GpioPort.h"

#include <cstddef>
#include <vector>

// ============================================================================
// CT117E-M4 competition board model (Beijing Guoxin Changtian, schematic
// SCH_CT117E_M4_V1.1/V1.2, STM32G431RBT6).
//
// LED matrix (verified against schematic + official driver code):
//   LD1..LD8 anodes -> 1k resistors -> 3V3, cathodes -> 74LS573 Q0..Q7.
//   74LS573 inputs D0..D7 = PC8..PC15 (shared with LCD data D8..D15),
//   LE (latch enable) = PD2, OE tied low. The latch is TRANSPARENT while
//   LE is high and HOLDS on the LE falling edge.
//   => LEDx lights when the latched PC(8+x) level is LOW (active low).
//
// Buttons (verified against schematic + user firmware):
//   B1=PB0  B2=PB1  B3=PB2  B4=PA0, each with a 10k external pull-up to
//   3V3 and a switch to GND: pressed = pin LOW, released = pin HIGH.
//
// LCD module (ILI9325 controller, 240x320 GRAM, mounted landscape 320x240;
// wiring verified against the official CT117E LCD BSP lcd.c):
//   GPIOC[15:0] -> LCD DB[15:0]      PB5 -> LCD_WR (NWR)
//   PB8         -> LCD_RS            PB9 -> LCD_CS (NCS)
//   PA8         -> LCD_RD (NRD)      LCD_RST follows the board reset
//   Board model job: mirror the pin levels into the controller and drive the
//   data bus back into GPIOC->IDR while the controller asserts NRD.
//
// Input path (hardware-accurate):
//   Qt button -> Board button state -> GPIO external drive -> GPIOx_IDR
//   -> firmware reads the register. The GUI never touches firmware state.
//
// TIM / analog board wiring (stages 3-4):
//   PA7  <- TIM3_CH2 / TIM17_CH1: the board only MONITORS the final pin
//           waveform (DigitalSignalMonitor) -- it never reads timer registers,
//           so a missing AF configuration really shows up as "no PWM".
//   PA15 <- PulseInputSource: the "external function generator" that replaces
//           the on-board XL555 (the analog circuit is deliberately NOT
//           modelled). Its level shares one GpioPort::setExternalInputAt()
//           call with button B4 (PA0), so the two external sources on GPIOA
//           never overwrite each other.
//   PB4  <- second PulseInputSource: the other 555 output on the board, used
//           as a second frequency input (TIM16_CH1 AF1 / TIM3_CH1 AF2). Shares
//           the GPIOB external-drive call with buttons B1..B3 (PB0..PB2).
//   PB15 -> ADC2_IN15 (R37 potentiometer)   -> AnalogSource r37_
//   PB12 -> ADC1_IN11 (R38 potentiometer)   -> AnalogSource r38_
//           (pin/channel assignment verified against the CT117E-M4 schematic /
//            product manual and the ST pin map: PB15 = ADC2_IN15,
//            PB12 = ADC1_IN11). The analog front end itself is not modelled:
//            the GUI sets the voltage, the board forwards it to the ADC
//            channel, the ADC converts, the firmware reads ADC_DR.
//
// Serial port (stage 5): the DAP-Link USB-serial port is wired to
//   PA9 = USART1_TX (AF7), PA10 = USART1_RX (AF7)
//   (schematic + product manual; the pin/AF numbers were cross-checked with
//    the ST pin map). VirtualSerialPeer is the PC-side terminal; the board
//   drives the RX line (idle high, low while a frame arrives) and forwards
//   finished MCU bytes. The SoC owns the AF gating, so a firmware that did not
//   route PA9/PA10 to AF7 really talks to nobody.
// ============================================================================
class Ct117eM4 : public IGpioWatcher {
public:
    explicit Ct117eM4(Stm32G431& soc);

    void attach();

    // ---- LEDs (latched) ----
    bool led(int index) const { return leds_[index]; }

    // ---- LCD module ----
    LcdController& lcd() { return lcd_; }
    const LcdController& lcd() const { return lcd_; }

    // ---- buttons (thread-safe, set from the GUI thread) ----
    void setButton(int index, bool pressed);
    bool button(int index) const;
    // Push the current button states AND the pulse input level into the GPIO
    // external-drive inputs (one call per port, so the sources on GPIOA can
    // never overwrite each other). Called by the simulator between batches.
    void applyInputs();

    // ---- pulse input source (PA15 -> TIM2_CH1 input capture) ----
    // GUI-facing, thread-safe: only the external pulse source is configurable,
    // never a timer register. Duty is fixed at 50 %.
    void setPulseEnabled(bool enabled);
    void setPulseFrequency(double hz);
    // Second pulse input on PB4 (the other 555 output on the board).
    void setPb4PulseEnabled(bool enabled);
    void setPb4PulseFrequency(double hz);
    // Engine path: apply pending configuration, advance the sources to the
    // current virtual time and push every edge onto the pin with its exact
    // cycle (so capture sees the real edge time).
    void advanceSignals();
    // HCLK cycles until the next pulse edge / serial frame transition of ANY
    // source (~0ull = none); the simulator slices its batches with this so an
    // edge is never crossed. @name reports WHICH board source owns it
    // (diagnostics: a short slice is what throttles the whole simulation).
    struct SignalSource {
        uint64_t cycles;
        const char* name;
    };
    SignalSource nextSignalEventSource() const;
    uint64_t cyclesToNextSignalEdge() const {
        return nextSignalEventSource().cycles;
    }
    const PulseInputSource& pulseSource() const { return pulse_; }
    const PulseInputSource& pb4PulseSource() const { return pulsePb4_; }

    // ---- pin waveform monitor (PA7, the PWM output) ----
    // Measured from the final pin waveform, never from timer registers.
    DigitalSignalMonitor::Stats pa7Waveform() const;

    // ---- analog inputs: R37 / R38 potentiometers (0 .. 3.3 V) ----
    void setR37Voltage(double volts) { r37_.setVoltage(volts); }
    void setR38Voltage(double volts) { r38_.setVoltage(volts); }
    double r37Voltage() const { return r37_.voltage(); }
    double r38Voltage() const { return r38_.voltage(); }

    // ---- serial port: virtual PC terminal (DAP-Link) on PA9/PA10 ----
    // PC -> MCU: queue bytes typed in the GUI terminal; the peer injects them
    // one frame at a time, spaced by the line rate the firmware configured.
    void serialSendToMcu(const std::vector<uint8_t>& bytes) {
        serialPeer_.sendToMcu(bytes.data(), bytes.size());
    }
    // MCU -> PC: the bytes that arrived since the last call (never the history)
    std::vector<uint8_t> serialTakeFromMcu() {
        return serialPeer_.takeFromMcu();
    }
    size_t serialRxPending() const { return serialPeer_.pendingToMcu(); }
    const VirtualSerialPeer& serialPeer() const { return serialPeer_; }

    // Board input ranges: the GUI reads these, it never hard-codes them.
    BoardProfile profile() const { return ct117eM4Profile(); }

    // ---- IGpioWatcher (engine thread, on every GPIO register write) ----
    void onGpioWrite(int portIndex) override;

private:
    void updateLedsFromPins();
    // Mirror GPIOA/B/C pin levels into the LCD module (data bus + control
    // lines) and reflect the read data back into GPIOC's input path.
    void syncLcdFromPins(bool allowLatch);
    // External-drive update stamped with @cpuCycle (exact edge time for
    // pulse-driven changes, "now" for register/GUI-driven changes).
    void applyInputsAt(uint64_t cpuCycle);

    Stm32G431& soc_;
    bool leds_[8] = {false, false, false, false, false, false, false, false};
    bool leLast_ = false;
    std::atomic<uint8_t> buttons_{0};

    // pulse input source: configured from the GUI, driven from the engine
    PulseInputSource pulse_;
    std::atomic<bool> pulseCfgDirty_{false};
    std::atomic<bool> pulseEnabled_{false};
    std::atomic<uint32_t> pulseFreqHz_{PulseInputSource::kDefaultFreqHz};
    // second pulse input (PB4, the other 555 output)
    PulseInputSource pulsePb4_;
    std::atomic<bool> pb4CfgDirty_{false};
    std::atomic<bool> pb4Enabled_{false};
    std::atomic<uint32_t> pb4FreqHz_{PulseInputSource::kDefaultFreqHz};

    // pin waveform monitor for the PWM output (PA7)
    DigitalSignalMonitor pwmMonA7_;

    // potentiometers (R37 -> ADC2_IN15 / PB15, R38 -> ADC1_IN11 / PB12)
    AnalogSource r37_, r38_;

    // virtual PC terminal on the DAP-Link serial port (USART1 PA9/PA10)
    VirtualSerialPeer serialPeer_;
    bool serialRxLevel_ = true;  // UART line idles high

    LcdController lcd_;
};