#pragma once

#include <cstdint>

#include <QMetaType>

// Immutable state snapshot copied from the engine thread to the GUI via a
// queued signal (thread-safe by construction: plain value).
struct SimSnapshot {
    bool firmwareLoaded = false;
    bool running = false;

    // Cortex-M4 core
    uint32_t pc = 0, sp = 0, lr = 0, xpsr = 0, ipsr = 0;
    uint32_t control = 0, primask = 0, basepri = 0;
    uint32_t regs[13] = {0};

    // virtual clock
    uint64_t cycles = 0;
    uint32_t sysclkHz = 0;

    // board
    bool leds[8] = {false, false, false, false, false, false, false, false};
    uint8_t buttons = 0;

    // peripherals of interest
    uint32_t systickCsr = 0, systickRvr = 0, systickCvr = 0;
    uint32_t gpioC_Odr = 0, gpioC_Idr = 0, gpioB_Idr = 0, gpioA_Idr = 0;
    uint32_t rccCr = 0, rccCfgr = 0;

    // LCD module (board device). The framebuffer itself is not copied here
    // (153 KB); the GUI uses it as a refresh hint and pulls the pixels with
    // Simulator::copyLcdFramebuffer() only when lcdDirty is set.
    bool lcdDirty = false;
    uint64_t lcdCommandWrites = 0;
    uint64_t lcdPixelWrites = 0;
    uint16_t lcdLastCommand = 0;

    // Board pin waveform monitor (PA7 = TIM3_CH2 PWM output). Measured from the
    // final pin waveform -- never derived from TIM registers, so a firmware
    // that forgot the AF configuration really reports "no PWM".
    bool pa7Active = false;
    bool pa7Level = false;
    double pa7Frequency = 0.0;  // Hz, 0 = no waveform
    double pa7Duty = 0.0;       // 0..1, meaningless when !pa7Active
    uint64_t pa7Edges = 0;

    // Pulse input sources (PA15 -> TIM2_CH1, PB4 -> TIM16_CH1). The GUI sets the
    // frequencies; the waveforms themselves are generated in virtual time.
    bool pulseEnabled = false;
    bool pulseLevel = false;
    double pulseFrequency = 0.0;  // Hz as configured
    bool pb4PulseEnabled = false;
    bool pb4PulseLevel = false;
    double pb4PulseFrequency = 0.0;

    // Speed / performance: virtual milliseconds per wall millisecond over the last
    // ~1 s window (1.0 = real time). A value near 0 with Cycles still rising
    // means "very slow", not "hung" -- see the [perf] lines in the GUI log.
    double realTimeX = 0.0;

    // Potentiometer positions (R37 -> ADC2_IN15, R38 -> ADC1_IN11)
    double r37Voltage = 0.0;
    double r38Voltage = 0.0;

    // USART1 / serial terminal (DAP-Link peer on PA9/PA10, AF7). Everything
    // here is read from the USART model -- the firmware configures it, the GUI
    // only displays it.
    bool usart1ClockEnabled = false;
    bool usart1Enabled = false;
    bool usart1TxEnabled = false;
    bool usart1RxEnabled = false;
    uint32_t usart1Baud = 0;       // decoded from BRR/PRESC/OVER8
    uint32_t usart1WordBits = 8;   // 7 / 8 / 9
    int usart1Parity = 0;          // 0 = none, 1 = even, 2 = odd
    uint32_t usart1StopBitsHalf = 2;  // 2 = 1 stop bit
    bool usart1Rxne = false, usart1Tc = false, usart1Ore = false;
    uint32_t usart1TxPending = 0;  // MCU -> PC bytes waiting for the terminal
    uint64_t usart1TxTotal = 0;    // bytes the MCU sent since reset
    uint64_t usart1RxTotal = 0;    // bytes the terminal sent since reset
    uint32_t usart1Queued = 0;    // PC -> MCU bytes not yet on the line

    // exception machinery
    int activeException = 0;
    uint64_t exceptionReturns = 0;

    // firmware info
    uint32_t fwLo = 0, fwHi = 0;
};

Q_DECLARE_METATYPE(SimSnapshot)
