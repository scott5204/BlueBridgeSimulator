#pragma once

#include <cstdint>
#include <functional>

#include "core/IBusDevice.h"

// ============================================================================
// STM32G4 RCC model (base 0x40021000, RM0440).
//
// Stage-1 scope: enough for a HAL/LL SystemClock_Config to complete and for
// the virtual clock to know the real bus frequencies:
//   - CR: ready flags mirror enable flags (HSI/HSE/PLL lock instantly)
//   - CFGR: SWS switch status mirrors SW selection
//   - PLLCFGR: stored; decoded for the clock tree
//   - all other registers: plain storage (incl. AHB2ENR GPIO clock gates)
//
// CT117E-M4 board oscillator: HSE = 24 MHz crystal (X1),
// HSI16 = 16 MHz (on-chip). PLL: f = src / M * N / R.
//   Example (competition standard): 24MHz /3 *20 /2 = 80 MHz.
// ============================================================================
class Rcc : public BusDevice32 {
public:
    static constexpr uint32_t kBase = 0x40021000u;
    static constexpr uint32_t kSize = 0xA0u;

    static constexpr uint32_t kHseHz = 24'000'000u;  // CT117E-M4 crystal
    static constexpr uint32_t kHsiHz = 16'000'000u;

    Rcc() : BusDevice32(kBase) {}
    void reset();

    // ---- clock tree ----
    uint32_t sysclkHz() const;
    uint32_t ahbHz() const;   // HCLK
    uint32_t apb1Hz() const;  // PCLK1
    uint32_t apb2Hz() const;  // PCLK2
    uint32_t apb1TimerHz() const;  // TIMPCLK1 (x2 when PPRE1 > 1)
    uint32_t apb2TimerHz() const;  // TIMPCLK2

    // ---- clock gates ----
    bool gpioClockEnabled(int port) const {
        return (ahb2enr_ >> port) & 1u;
    }
    uint32_t ahb2enr() const { return ahb2enr_; }
    // fired after AHB2ENR writes so the SoC can update the GPIO ports
    std::function<void()> onAhb2enrChanged;

    // APB2 gates (APB2ENR, offset 0x60). USART1EN = bit 14 (RM0440):
    // an unclocked USART reads as 0 and drops every write.
    uint32_t apb2enr() const { return regs_[0x60 / 4]; }
    bool usart1ClockEnabled() const { return (apb2enr() >> 14) & 1u; }
    // fired after APB2ENR writes so the SoC can update the APB2 peripherals
    std::function<void()> onApb2enrChanged;

protected:
    uint32_t readReg(uint32_t regOff) override;
    void writeReg(uint32_t regOff, uint32_t value) override;

public:
    // side-effect-free read for debugger/snapshot views
    uint32_t debugReadReg(uint32_t regOff) const;

private:
    // storage for the whole register block
    uint32_t regs_[kSize / 4] = {0};

    // frequently accessed values cached
    uint32_t cr_ = 0;
    uint32_t cfgr_ = 0;
    uint32_t pllcfgr_ = 0;
    uint32_t ahb2enr_ = 0;
};
