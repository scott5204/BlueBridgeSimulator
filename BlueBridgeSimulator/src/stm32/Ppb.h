#pragma once

#include <cstdint>
#include <functional>

#include "core/IBusDevice.h"
#include "stm32/nvic/NvicController.h"
#include "stm32/systick/SysTickTimer.h"

// ============================================================================
// ARMv7-M Private Peripheral Bus (PPB) device at 0xE000E000..0xE000EFFF.
//
//   SysTick : 0x10 CSR / 0x14 RVR / 0x18 CVR / 0x1C CALIB
//   NVIC    : 0x100 ISER / 0x180 ICER / 0x200 ISPR / 0x280 ICPR / 0x400 IPR
//   SCB     : 0xD00 CPUID / 0xD04 ICSR / 0xD08 VTOR / 0xD0C AIRCR /
//             0xD10 SCR / 0xD14 CCR / 0xD18-0xD23 SHPR / 0xD24 SHCSR /
//             0xD88 CPACR / 0xDFC DEMCR
//   STIR    : 0xF00
//
// Exception *entry/return* is executed by the Simulator (manual NVIC
// injection), this class only holds the architectural state.
// ============================================================================
class Ppb : public BusDevice32 {
public:
    static constexpr uint32_t kBase = 0xE000E000u;
    static constexpr uint32_t kSize = 0x1000u;

    Ppb() : BusDevice32(kBase) {}
    SysTickTimer systick;
    NvicController nvic;

    void reset();

    uint32_t vtor() const { return vtor_; }
    uint32_t cpacr() const { return cpacr_; }
    uint32_t demcr() const { return demcr_; }
    uint32_t aircr() const { return aircr_; }

    // set by Simulator: called when firmware writes CPACR (FPU enable),
    // so the CPU core state can be updated too
    std::function<void(uint32_t)> cpacrChanged;
    // set by Simulator: AIRCR.SYSRESETREQ handling
    std::function<void()> sysResetRequested;
    // set by Simulator: NMI pended via ICSR
    std::function<void()> nmiPended;

protected:
    uint32_t readReg(uint32_t regOff) override;
    void writeReg(uint32_t regOff, uint32_t value) override;

private:
    uint32_t vtor_ = 0;
    uint32_t cpacr_ = 0;
    uint32_t demcr_ = 0;
    uint32_t aircr_ = 0x0000'F000u;  // PRIGROUP=3 (all preemption)
    uint32_t scr_ = 0;
    uint32_t ccr_ = 0;
    uint32_t shcsr_ = 0;
    uint32_t icsrPendMask_ = 0;  // unused storage bits
};
