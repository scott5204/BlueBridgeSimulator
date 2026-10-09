#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/MemoryBus.h"
#include "debug/DebugTypes.h"
#include "loader/HexLoader.h"
#include "stm32/Ppb.h"
#include "stm32/SimpleDevices.h"
#include "stm32/adc/Adc.h"
#include "stm32/gpio/GpioPort.h"
#include "stm32/rcc/Rcc.h"
#include "stm32/tim/Timer.h"
#include "stm32/usart/Usart.h"

// ============================================================================
// STM32G431RBT6 system-on-chip model (CT117E-M4 main controller).
//
// Memory map (RM0440 + STM32G431 datasheet):
//   Flash   0x08000000  128 KB   (aliased at 0x00000000 after reset)
//   SRAM    0x20000000   32 KB   (SRAM1 16K + SRAM2 6K = 22K on real part;
//                                 extra headroom is harmless)
//   CCMSRAM 0x10000000   16 KB
//   APB1    0x40000000 .. 0x4000FFFF  (TIM2/3/4/6/7, USART2/3/4, PWR, ...)
//   APB2    0x40010000 .. 0x4001FFFF  (SYSCFG, EXTI, TIM1/8/15/16, USART1, ...)
//   AHB1    0x40020000 .. 0x40023FFF  (DMA1/2, RCC, FLASH, CRC)
//   AHB2    0x48000000 .. 0x48001FFF  (GPIOA..GPIOE)
//   PPB     0xE000E000                    (SysTick, NVIC, SCB)
//   DWT     0xE0001000, ITM 0xE0000000
//
// Modeled peripherals: RCC (clock tree + gates), GPIOA-E, SysTick, NVIC, SCB,
// DWT cycle counter, ITM printf channel, TIM1/2/3/4/6/7/8/15/16/17, ADC1/ADC2
// (+ ADC12_COMMON) and USART1. Everything else falls through to the bus
// scratch storage, so real firmware that touches other peripherals still runs
// (without side effects).
// ============================================================================
class Stm32G431 {
public:
    static constexpr uint32_t kFlashBase = 0x08000000u;
    static constexpr uint32_t kFlashSize = 128u * 1024u;
    static constexpr uint32_t kSramBase = 0x20000000u;
    static constexpr uint32_t kSramSize = 32u * 1024u;
    static constexpr uint32_t kCcmBase = 0x10000000u;
    static constexpr uint32_t kCcmSize = 16u * 1024u;

    Stm32G431();

    // Full peripheral reset (SRAM is cleared, flash kept).
    void reset();

    // Load a firmware image into flash (overwrites flash content).
    bool loadFirmware(const FirmwareImage& img, std::string& error);
    // Build a full flash image (exactly kFlashSize bytes, 0xFF outside the
    // image segments); false when the image is empty or falls outside flash.
    static bool buildFlashImage(const FirmwareImage& img, uint8_t* out,
                                size_t size, std::string& error);
    // Replace the whole live flash content. This is the SINGLE backing store
    // the CPU executes from (the 0x00000000 alias is derived from it -- there
    // is no second flash copy). @size must be kFlashSize.
    bool replaceFlash(const uint8_t* data, size_t size);

    uint8_t* flash() { return flash_; }
    uint8_t* sram() { return sram_; }
    uint8_t* ccm() { return ccm_; }

    // ---- direct memory access (vector fetch, debugger, self tests) ----
    // Handles the 0x00000000 flash alias and the real memory regions;
    // MMIO is not routed through this path.
    bool readWord(uint32_t addr, uint32_t& out) const;
    bool writeWord(uint32_t addr, uint32_t value);

    // ---- host (debugger) guest memory access ------------------------------
    // Byte-granular access with exactly the semantics the CPU sees:
    //   flash (incl. the 0x00000000 boot alias), SRAM, CCM -> backing memory
    //   peripheral space -> the normal bus model (a read may have the same
    //   side effects as a CPU read; this is intentional, see PROJECT_HANDOFF)
    //   flash write -> Unsupported (no flash programming protocol yet)
    //   anything else -> InvalidAddress (never throws, never crashes)
    DebugStatus guestRead(uint32_t addr, void* data, size_t size);
    DebugStatus guestWrite(uint32_t addr, const void* data, size_t size);
    // True when [addr, addr+size) lies in the peripheral (MMIO) window the bus
    // serves, including the unimplemented-address scratch area.
    static bool isPeripheralAddress(uint32_t addr, size_t size);

    // ---- CPU-facing MMIO (installed as unicorn hooks by the Simulator) ----
    uint32_t mmioRead(uint32_t addr, uint32_t size) {
        return bus_.read(addr, size);
    }
    void mmioWrite(uint32_t addr, uint32_t value, uint32_t size) {
        bus_.write(addr, value, size);
    }

    // ---- virtual clock ----
    // Advance the peripheral world by @cycles HCLK cycles.
    void advanceTime(uint64_t cycles);
    // See advanceTime(): the scheduler never passes more than one batch, so any
    // larger delta is corrupt and is clamped (a freeze guard, not a limit).
    static constexpr uint64_t kMaxAdvanceDelta = 1u << 24;  // 16.7 M cycles
    bool hugeDeltaReported_ = false;
    // HCLK cycles until the next schedulable peripheral event (SysTick)
    // Next schedulable peripheral event, HCLK cycles (~0 = none). The batch
    // budget never crosses it; @name reports WHICH peripheral produced it
    // (diagnostics: a short slice is what throttles the whole simulation).
    struct EventSource {
        uint64_t cycles;
        const char* name;
    };
    EventSource nextEventSource() const;
    uint64_t cyclesToNextEvent() const { return nextEventSource().cycles; }
    uint64_t totalCycles() const { return cycles_; }

    // ---- clock tree ----
    uint32_t sysclkHz() const { return rcc.sysclkHz(); }
    uint32_t ahbHz() const { return rcc.ahbHz(); }

    // SysReset via AIRCR.SYSRESETREQ
    bool consumeResetRequest() {
        bool r = resetRequested_;
        resetRequested_ = false;
        return r;
    }

    void setLogger(std::function<void(const std::string&)> logger) {
        busLogger_ = std::move(logger);
        bus_.setLogger(busLogger_);
    }

    // ---- devices (public: wired by the board model) ----
    Rcc rcc;
    GpioPort gpioA{0}, gpioB{1}, gpioC{2}, gpioD{3}, gpioE{4};
    Ppb ppb;
    DwtDevice dwt;
    ItmDevice itm;

    // ---- timers (RM0440 bases / IRQn from docs/stm32g431xx.h) ----
    //              base        name      ch  adv   32b  IRQn APB
    Timer tim1{0x40012C00u, {"TIM1", 4, true, false, 25, 2}};   // APB2, MOE
    Timer tim2{0x40000000u, {"TIM2", 4, false, true, 28, 1}};   // 32-bit
    Timer tim3{0x40000400u, {"TIM3", 4, false, false, 29, 1}};
    Timer tim4{0x40000800u, {"TIM4", 4, false, false, 30, 1}};
    Timer tim6{0x40001000u, {"TIM6", 0, false, false, 54, 1}};  // basic
    Timer tim7{0x40001400u, {"TIM7", 0, false, false, 55, 1}};  // basic
    Timer tim8{0x40013400u, {"TIM8", 4, true, false, 44, 2}};   // MOE
    Timer tim15{0x40014000u, {"TIM15", 2, true, false, 24, 2}}; // MOE
    Timer tim16{0x40014400u, {"TIM16", 1, true, false, 25, 2}}; // MOE
    Timer tim17{0x40014800u, {"TIM17", 1, true, false, 26, 2}}; // MOE

    static constexpr int kTimerCount = 10;
    Timer* timerAt(int i) { return timers_[i]; }
    const Timer* timerAt(int i) const { return timers_[i]; }
    Timer* timerByName(const char* name);
    GpioPort& gpio(int port) { return *portPtr(port); }

    // ---- ADCs (RM0440 bases; ADC1_2_IRQn = 18) ----
    //             base         name    index irq
    Adc adc1{0x50000000u, {"ADC1", 1, 18}};
    Adc adc2{0x50000100u, {"ADC2", 2, 18}};
    AdcCommonRegs adcCommon;  // ADC12_COMMON (clock config / common data)
    Adc* adcAt(int index) { return index == 2 ? &adc2 : &adc1; }
    const Adc* adcAt(int index) const { return index == 2 ? &adc2 : &adc1; }

    // ---- USART1 (RM0440 base; USART1_IRQn = 37) ----
    // PA9 = USART1_TX AF7 / PA10 = USART1_RX AF7 (PB6/PB7, PC4/PC5 are the
    // alternates) -- the DAP-Link USB-serial port of the CT117E-M4.
    Usart usart1{0x40013800u, {"USART1", 37}};

    // Byte-level serial line (stage 5). The BOARD's virtual PC peer attaches
    // its receiver here; the SoC forwards an MCU byte only while the firmware
    // really routed a TX pin to USART1 (MODER = AF, AF7 -- kUsartAfTable), so
    // a wrong AF loses the bytes exactly like on real hardware.
    void setUsart1LineReceiver(std::function<void(uint8_t)> fn) {
        usartLineRx_ = std::move(fn);
    }
    // PC peer -> MCU. Returns true when the byte entered the USART (RX pin
    // routed to USART1_RX); a wrong RX AF really leaves RXNE at 0.
    bool usart1RxFromLine(uint8_t b);
    bool usart1TxRouted() const { return usartPinsRouted(true); }
    bool usart1RxRouted() const { return usartPinsRouted(false); }

    // Timer input clock from the RCC clock tree (APB prescaler + x2 rule).
    // Never hard-coded: firmware RCC changes are picked up automatically.
    uint32_t timerClockHz(int apb) const {
        return apb == 2 ? rcc.apb2TimerHz() : rcc.apb1TimerHz();
    }

    // Timer tracing (development aid, off by default)
    void setTimerLogger(std::function<void(const std::string&)> logger);
    void setTimerTrace(bool events, bool regs);

private:
    // Alternate-function routing: TIMx_CHy <-> GPIO pin (table in the .cpp,
    // derived from the CubeMX G431R database and the ST pin map). The timer
    // itself knows only channels; the GPIO knows only pins.
    struct AfBinding {
        int port;
        int pin;
        int af;
        Timer* timer;
        int channel;
    };
    void wireTimers();
    void wireAnalog();
    void wireUsart();
    bool usartPinsRouted(bool tx) const;
    GpioPort* portPtr(int port);
    const GpioPort* portPtr(int port) const;
    // real (non-MMIO) guest memory at [addr, addr+size), or nullptr
    const uint8_t* realMemoryAt(uint32_t addr, size_t size) const;
    uint8_t* realMemoryAt(uint32_t addr, size_t size);

    void buildBus();

    uint8_t flash_[kFlashSize];
    uint8_t sram_[kSramSize];
    uint8_t ccm_[kCcmSize];

    MemoryBus bus_;
    std::function<void(const std::string&)> busLogger_;
    std::function<void(uint8_t)> usartLineRx_;  // MCU -> PC peer (board)
    uint64_t cycles_ = 0;
    bool resetRequested_ = false;

    Timer* timers_[kTimerCount] = {};
};
