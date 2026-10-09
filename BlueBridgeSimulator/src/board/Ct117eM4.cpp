#include "board/Ct117eM4.h"

#include <algorithm>

namespace {
// CT117E-M4 LCD wiring (official BSP lcd.c + schematic)
constexpr int kLcdCsPin = 9;  // PB9, active low
constexpr int kLcdRsPin = 8;  // PB8, 1 = data, 0 = command
constexpr int kLcdWrPin = 5;  // PB5, latches on falling edge
constexpr int kLcdRdPin = 8;  // PA8, active low

// TIM-related board pins
constexpr int kPulsePin = 15;   // PA15 -> TIM2_CH1 (input capture / pulse in)
constexpr int kPulsePb4Pin = 4; // PB4  -> TIM16_CH1 (AF1) / TIM3_CH1 (AF2)
constexpr int kPwmOutPin = 7;   // PA7  <- TIM3_CH2 (PWM, waveform monitor)

// Analog inputs (verified: CT117E-M4 schematic + product manual + ST pin map)
//   R37 potentiometer -> PB15 -> ADC2 channel 15
//   R38 potentiometer -> PB12 -> ADC1 channel 11
constexpr int kR37Adc = 2, kR37Channel = 15;
constexpr int kR38Adc = 1, kR38Channel = 11;

// Serial port (DAP-Link USB-serial, verified: schematic + product manual +
// ST pin map): PA9 = USART1_TX AF7, PA10 = USART1_RX AF7. The board drives
// the RX line (the DAP-Link's TX output) and listens to the MCU's TX.
constexpr int kSerialRxPin = 10;  // PA10 (driven by the peer)
}  // namespace

Ct117eM4::Ct117eM4(Stm32G431& soc) : soc_(soc) {
    // Board waveform monitor on the PWM output pin. addLevelObserver() and
    // never setLevelObserver(): PA7 also carries the SoC's timer capture input
    // routing (TIM3_CH2 / TIM17_CH1), which must stay installed.
    soc_.gpioA.addLevelObserver(kPwmOutPin,
                                [this](bool level, uint64_t cpuCycle) {
                                    pwmMonA7_.onLevel(level, cpuCycle);
                                });

    // Analog sources -> ADC channels. The ADC model asks for the voltage of
    // the channel its sequencer selected; the board is the only place that
    // knows which pin (and therefore which voltage) that channel belongs to.
    auto voltageOf = [this](int adcIndex, int channel) -> double {
        if (adcIndex == kR37Adc && channel == kR37Channel) return r37_.voltage();
        if (adcIndex == kR38Adc && channel == kR38Channel) return r38_.voltage();
        return 0.0;  // unrouted channel: 0 V
    };
    soc_.adc1.setChannelVoltageProvider(
        [voltageOf](int ch) { return voltageOf(1, ch); });
    soc_.adc2.setChannelVoltageProvider(
        [voltageOf](int ch) { return voltageOf(2, ch); });

    // Potentiometer range = board analog range (0 .. 3.3 V)
    const BoardProfile prof = ct117eM4Profile();
    r37_.setRange(prof.analogMinVoltage, prof.analogMaxVoltage);
    r38_.setRange(prof.analogMinVoltage, prof.analogMaxVoltage);

    // Serial port wiring: MCU bytes -> virtual PC terminal. The SoC applies
    // the TX pin routing, so this sink only sees bytes that really left PA9
    // (or PB6/PC4) on USART1_TX.
    soc_.setUsart1LineReceiver([this](uint8_t b) {
        serialPeer_.onByteFromMcu(b);
    });
}

void Ct117eM4::attach() {
    soc_.gpioA.setWatcher(this);
    soc_.gpioB.setWatcher(this);
    soc_.gpioC.setWatcher(this);
    soc_.gpioD.setWatcher(this);
    soc_.gpioE.setWatcher(this);
    applyInputs();
    leLast_ = false;
    for (int i = 0; i < 8; i++) leds_[i] = false;

    // Board reset also resets the LCD module (its /RESET follows the board
    // reset): controller back to reset state, firmware re-initialises it.
    lcd_.reset();
    syncLcdFromPins(/*allowLatch=*/false);

    // Waveform statistics restart with the board. The external pulse source is an
    // instrument, not a peripheral: its configuration (GUI settings) survives,
    // only the phase restarts. Same for the potentiometer positions.
    pwmMonA7_.reset();
    pulse_.restartPhase(soc_.totalCycles(), soc_.ahbHz());
    pulsePb4_.restartPhase(soc_.totalCycles(), soc_.ahbHz());

    // The serial terminal is an instrument too: a board reset does not clear
    // the terminal history, it only restarts the byte timeline. The RX line
    // idles high (the DAP-Link drives it even while the firmware is not
    // configured yet -- exactly like the real wire).
    serialPeer_.reset(soc_.totalCycles());
    serialRxLevel_ = true;
}

void Ct117eM4::setButton(int index, bool pressed) {
    if (index < 0 || index > 3) return;
    uint8_t mask = uint8_t(1u << index);
    if (pressed)
        buttons_.fetch_or(mask);
    else
        buttons_.fetch_and(uint8_t(~mask));
}

bool Ct117eM4::button(int index) const {
    if (index < 0 || index > 3) return false;
    return (buttons_.load() >> index) & 1u;
}

void Ct117eM4::setPulseEnabled(bool enabled) {
    pulseEnabled_.store(enabled);
    pulseCfgDirty_.store(true);
}

void Ct117eM4::setPulseFrequency(double hz) {
    pulseFreqHz_.store(PulseInputSource::clampFreq(
        uint32_t(hz < 0.0 ? 0.0 : hz + 0.5), soc_.ahbHz()));
    pulseCfgDirty_.store(true);
}

void Ct117eM4::setPb4PulseEnabled(bool enabled) {
    pb4Enabled_.store(enabled);
    pb4CfgDirty_.store(true);
}

void Ct117eM4::setPb4PulseFrequency(double hz) {
    pb4FreqHz_.store(PulseInputSource::clampFreq(
        uint32_t(hz < 0.0 ? 0.0 : hz + 0.5), soc_.ahbHz()));
    pb4CfgDirty_.store(true);
}

void Ct117eM4::advanceSignals() {
    const uint64_t now = soc_.totalCycles();
    const uint32_t hclk = soc_.ahbHz();

    if (pulseCfgDirty_.exchange(false)) {
        pulse_.setFrequencyHz(pulseFreqHz_.load(), now, hclk);
        pulse_.setEnabled(pulseEnabled_.load(), now, hclk);
    }
    if (pb4CfgDirty_.exchange(false)) {
        pulsePb4_.setFrequencyHz(pb4FreqHz_.load(), now, hclk);
        pulsePb4_.setEnabled(pb4Enabled_.load(), now, hclk);
    }

    // Every edge is written to its GPIO port at its exact cycle, so the pin
    // observers (timer capture inputs) latch the counter value of the real
    // edge time.
    pulse_.advance(now, hclk, [this](bool, uint64_t cpuCycle) {
        applyInputsAt(cpuCycle);
    });
    pulsePb4_.advance(now, hclk, [this](bool, uint64_t cpuCycle) {
        applyInputsAt(cpuCycle);
    });

    // Serial peer: the line rate comes from the firmware's BRR (decoded by the
    // USART model), the PC -> MCU bytes keep their frame times, the RX pin is
    // driven with the exact cycle of every frame transition and the finished
    // frames are handed to the SoC (which applies the RX pin routing).
    serialPeer_.setBaud(soc_.usart1.baudRate());
    serialPeer_.advance(
        now, hclk,
        [this](bool level, uint64_t cpuCycle) {
            serialRxLevel_ = level;
            applyInputsAt(cpuCycle);
        },
        [this](uint8_t b) { soc_.usart1RxFromLine(b); });
}

Ct117eM4::SignalSource Ct117eM4::nextSignalEventSource() const {
    if (pulseCfgDirty_.load() || pb4CfgDirty_.load())
        return {0, "generator cfg"};  // a new configuration is waiting
    SignalSource s{~0ull, "none"};
    if (pulse_.enabled()) {
        const uint64_t e = pulse_.cyclesToNextEdge();
        if (e < s.cycles) s = {e, "PA15 gen"};
    }
    if (pulsePb4_.enabled()) {
        const uint64_t e = pulsePb4_.cyclesToNextEdge();
        if (e < s.cycles) s = {e, "PB4 gen"};
    }
    // serial frames must be sliced too: a byte has to reach the USART (and the
    // RX pin has to change) at its own cycle
    const uint64_t p = serialPeer_.cyclesToNextEvent();
    if (p < s.cycles) s = {p, "serial peer"};
    return s;
}

DigitalSignalMonitor::Stats Ct117eM4::pa7Waveform() const {
    return pwmMonA7_.measure(soc_.totalCycles(), soc_.ahbHz());
}

void Ct117eM4::applyInputs() { applyInputsAt(soc_.totalCycles()); }

void Ct117eM4::applyInputsAt(uint64_t cpuCycle) {
    const uint8_t b = buttons_.load();
    // GPIOB pins 0..2 = B1..B3, PB4 = second pulse input source.
    // Released: external 10k pull-up drives the pin high.
    // Pressed:  switch closes to GND, drives the pin low.
    const uint16_t pbMask = 0b0000'0111;
    const uint16_t pbVal = uint16_t((~b & 0b111) & 0b111);

    // GPIOA: PA0 = B4, PA15 = pulse input source. A single call covers both
    // pins; a source only drives its pin while it is enabled. PA10 = the
    // DAP-Link serial RX line (always driven: it idles high).
    uint16_t paMask = 0b0001;
    uint16_t paVal = uint16_t(((~b >> 3) & 1u) & 0b1);
    if (pulse_.drivesPin()) {
        paMask |= uint16_t(1u << kPulsePin);
        if (pulse_.level()) paVal |= uint16_t(1u << kPulsePin);
    }
    paMask |= uint16_t(1u << kSerialRxPin);
    if (serialRxLevel_) paVal |= uint16_t(1u << kSerialRxPin);

    // PB4: the second pulse source shares the GPIOB call with the buttons
    uint16_t pbMaskAll = pbMask;
    uint16_t pbValAll = pbVal;
    if (pulsePb4_.drivesPin()) {
        pbMaskAll |= uint16_t(1u << kPulsePb4Pin);
        if (pulsePb4_.level()) pbValAll |= uint16_t(1u << kPulsePb4Pin);
    }

    soc_.gpioB.setExternalInput(pbMaskAll, pbValAll);
    soc_.gpioA.setExternalInputAt(paMask, paVal, cpuCycle);
}

void Ct117eM4::onGpioWrite(int portIndex) {
    (void)portIndex;
    // 74LS573: transparent latch. While LE (PD2) is high, outputs follow
    // inputs (PC8..PC15); on the falling edge the outputs hold.
    const bool le = (soc_.gpioD.pinLevels() >> 2) & 1u;
    if (le) {
        updateLedsFromPins();
    }
    leLast_ = le;

    // Any GPIO write can change an LCD bus line or the data bus, so mirror
    // the levels into the module on every write (the module latches on the
    // WR falling edge itself).
    syncLcdFromPins(/*allowLatch=*/true);
}

void Ct117eM4::updateLedsFromPins() {
    const uint16_t pc = soc_.gpioC.pinLevels();
    for (int i = 0; i < 8; i++) {
        // active low: LEDx on when PC(8+x) is low
        leds_[i] = !((pc >> (8 + i)) & 1u);
    }
}

void Ct117eM4::syncLcdFromPins(bool allowLatch) {
    const uint16_t pc = soc_.gpioC.pinLevels();
    const uint16_t pb = soc_.gpioB.pinLevels();
    const uint16_t pa = soc_.gpioA.pinLevels();

    // The module is fed with the raw pin levels (true = HIGH); it knows the
    // polarity of each line (NCS/NRD active low, RS high = data, ...).
    lcd_.setDataBus(pc);
    lcd_.setChipSelect(((pb >> kLcdCsPin) & 1u) != 0);
    lcd_.setRegisterSelect(((pb >> kLcdRsPin) & 1u) != 0);
    lcd_.setRead(((pa >> kLcdRdPin) & 1u) != 0);
    // WR last: its falling edge latches the data bus and RS level above.
    if (allowLatch) {
        lcd_.setWrite(((pb >> kLcdWrPin) & 1u) != 0);
    } else {
        // Initial line sampling after reset: no phantom latch on the first
        // level change (the pins may be floating/analog right after reset).
        lcd_.primeWriteLine(((pb >> kLcdWrPin) & 1u) != 0);
    }

    // While the controller drives the bus (NRD low, selected), the MCU reads
    // it through GPIOC->IDR exactly like on the real board.
    if (lcd_.drivingBus()) {
        soc_.gpioC.setExternalInput(0xFFFFu, lcd_.readDataBus());
    } else {
        soc_.gpioC.setExternalInput(0x0000u, 0x0000u);
    }
}