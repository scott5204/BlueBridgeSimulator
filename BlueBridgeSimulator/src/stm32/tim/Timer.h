#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "core/IBusDevice.h"

// ============================================================================
// STM32G431 timer model (RM0440: general-purpose TIM2/TIM3/TIM4, advanced
// TIM1/TIM8/TIM20/TIM15/TIM16/TIM17, basic TIM6/TIM7).
//
// Implemented (stage 3):
//   CR1 (CEN/UDIS/URS/DIR/CMS/ARPE), CR2, SMCR (stored)
//   DIER, SR (write-0-to-clear), EGR (UG/CCxG)
//   CCMR1/CCMR2 (+CCxS/OCxM/OCxPE), CCER (CCxE/CCxP/CCxNP)
//   CNT, PSC (buffered), ARR (buffered with ARPE), CCR1..4, BDTR (MOE)
//   up-counting counter, update event + interrupt, PWM mode 1/2 output,
//   input capture (rising / falling / both edges), capture interrupt
//   exact (integer, drift-free) time base derived from the RCC timer clock
//
// Not implemented (stored, no behavioural effect -- a firmware using them is
// reported once through the logger): down / center-aligned counting, encoder
// and slave/master modes, external clock mode, complementary outputs, dead
// time, break, one-pulse mode, DMA burst, input filters and capture
// prescalers (ICxF/ICxPSC are stored only).
//
// The timer knows nothing about pins or about the board: channel outputs are
// published through the channel callback (driven/level + exact CPU cycle) and
// channel inputs arrive via onChannelPinLevel(). Stm32G431 owns the AF table
// that connects TIMx_CHy to a GPIO pin.
// ============================================================================
class Timer : public BusDevice32 {
public:
    struct Traits {
        const char* name;
        int channels;    // 0 (TIM6/7), 1 (TIM16/17), 2 (TIM15), 4
        bool advanced;   // outputs gated by BDTR.MOE (TIM1/8/15/16/17/20)
        bool bit32;      // TIM2 has a 32-bit counter
        int irq;         // NVIC IRQn (0-based), -1 = none
        int apb;         // 1 = APB1 (pclk1), 2 = APB2 (pclk2)
    };

    Timer(uint32_t base, Traits traits);

    // ---- register indices (offsets) ----
    static constexpr uint32_t R_CR1 = 0x00, R_CR2 = 0x04, R_SMCR = 0x08;
    static constexpr uint32_t R_DIER = 0x0C, R_SR = 0x10, R_EGR = 0x14;
    static constexpr uint32_t R_CCMR1 = 0x18, R_CCMR2 = 0x1C, R_CCER = 0x20;
    static constexpr uint32_t R_CNT = 0x24, R_PSC = 0x28, R_ARR = 0x2C;
    static constexpr uint32_t R_RCR = 0x30, R_CCR1 = 0x34;
    static constexpr uint32_t R_BDTR = 0x44, R_DCR = 0x48, R_DMAR = 0x4C;
    static constexpr uint32_t R_AF1 = 0x60, R_AF2 = 0x64, R_TISEL = 0x68;

    void reset();

    // ---- time base ----
    // Advance by @cpuCycles HCLK cycles ending at absolute @endCpuCycle.
    // @timHz is the timer input clock from the RCC clock tree (APB timer
    // clock with the x2 rule already applied).
    void advance(uint64_t endCpuCycle, uint64_t cpuCycles, uint32_t hclk,
                 uint32_t timHz);
    // HCLK cycles until the next IRQ-generating event (~0ull = never)
    uint64_t cpuCyclesToNextIrq(uint32_t hclk, uint32_t timHz) const;

    // ---- channel routing (used by Stm32G431 for the AF table) ----
    // driven = the timer currently drives the pin, level = driven level,
    // cpuCycle = exact time of the change (for pin waveform monitors).
    using ChannelOutFn =
        std::function<void(int ch, bool driven, bool level, uint64_t cpuCycle)>;
    void setChannelOutputCallback(ChannelOutFn fn) {
        channelOut_ = std::move(fn);
    }
    // pin level change arriving at channel @ch (capture input path)
    void onChannelPinLevel(int ch, bool level, uint64_t cpuCycle);

    // Re-publish every channel output with the current level. Needed after the
    // firmware changed a GPIO routing register (MODER/AFR): the timer level
    // itself did not change, but the pin it reaches may have.
    void refreshChannelOutputs();

    // ---- NVIC ----
    void setIrqCallback(std::function<void(int irq)> fn) {
        irqCallback_ = std::move(fn);
    }

    // ---- tracing (development aid, off by default) ----
    void setLogger(std::function<void(const std::string&)> fn) {
        logger_ = std::move(fn);
    }
    void setTraceEvents(bool on) { traceEvents_ = on; }
    void setTraceRegs(bool on) { traceRegs_ = on; }

    // ---- inspection (unit tests / board code) ----
    const Traits& traits() const { return traits_; }
    bool enabled() const { return (cr1_ & kCr1Cen) != 0; }
    uint32_t counter() const { return cnt_; }
    uint32_t arr() const { return arrPre_; }
    uint32_t prescaler() const { return pscPre_; }
    uint32_t status() const { return sr_; }
    uint32_t ccr(int ch) const { return ch < 4 ? ccrPre_[ch] : 0; }
    bool outputDriven(int ch) const;
    bool outputLevel(int ch) const;

private:
    // CR1 bits
    static constexpr uint32_t kCr1Cen = 1u << 0, kCr1Udis = 1u << 1,
                             kCr1Urs = 1u << 2, kCr1Opm = 1u << 3,
                             kCr1Dir = 1u << 4, kCr1Cms = 0x3u << 5,
                             kCr1Arpe = 1u << 7;
    // SR / DIER bits
    static constexpr uint32_t kUif = 1u << 0, kCc1if = 1u << 1,
                             kCc2if = 1u << 2, kCc3if = 1u << 3,
                             kCc4if = 1u << 4;
    static constexpr uint32_t kUie = 1u << 0;
    // EGR bits
    static constexpr uint32_t kEgUg = 1u << 0;
    static constexpr uint32_t kEgCc1g = 1u << 1;
    // CCMR / CCER helpers
    static uint32_t ccmrByte(uint32_t ccmr, int chInPair);
    static constexpr uint32_t kOcModeMask = 0x7u;
    static constexpr uint32_t kOcModePwm1 = 0x6u, kOcModePwm2 = 0x7u;
    static bool channelIsOutput(uint32_t ccmr, int chInPair);
    static uint32_t channelMode(uint32_t ccmr, int chInPair);

    uint32_t readReg(uint32_t regOff) override;
    void writeReg(uint32_t regOff, uint32_t value) override;

    uint32_t counterMask() const { return traits_.bit32 ? 0xFFFFFFFFu : 0xFFFFu; }
    uint32_t ccmrFor(int ch) const { return ch < 2 ? ccmr1_ : ccmr2_; }
    int chInPair(int ch) const { return ch & 1; }
    bool channelOutputDriven(int ch) const;   // CCMR+CCER+MOE gate
    bool channelPolarity(int ch) const;       // CCxP
    bool channelEnabled(int ch) const;        // CCxE
    bool channelOutputActiveLevel(int ch) const;
    bool channelIsCapture(int ch) const;
    bool captureEdgeMatches(int ch, bool level) const;

    void onUpdateEvent(bool fromUg);
    void updateAllChannelOutputs(uint64_t cpuCycle);
    void publishChannel(int ch, bool force, uint64_t cpuCycle);
    void fireIrq();
    void traceEvent(const std::string& s) const;

    Traits traits_;

    // registers
    uint32_t cr1_ = 0, cr2_ = 0, smcr_ = 0, dier_ = 0, sr_ = 0;
    uint32_t ccmr1_ = 0, ccmr2_ = 0, ccer_ = 0, bdtr_ = 0;
    uint32_t cnt_ = 0;
    uint32_t pscPre_ = 0, pscAct_ = 0;     // PSC is buffered (preload)
    uint32_t arrPre_ = 0xFFFF, arrAct_ = 0xFFFF;
    uint32_t ccrPre_[4] = {0, 0, 0, 0};    // preload value (what the CPU reads)
    uint32_t ccrAct_[4] = {0, 0, 0, 0};    // value used for compare/capture
    uint32_t ccrCaptured_[4] = {0, 0, 0, 0};
    uint32_t rcr_ = 0, dcr_ = 0, dmar_ = 0, af1_ = 0, af2_ = 0, tisel_ = 0;
    uint32_t rep_ = 0;                     // repetition counter (advanced)

    // time base (integer, drift free)
    uint64_t clockAcc_ = 0;   // fractional timer-clock accumulator
    uint64_t prescAcc_ = 0;   // fractional counter-tick accumulator
    uint64_t tickCpuAcc_ = 0; // fractional CPU-cycle accumulator
    uint64_t cpuCursor_ = 0;  // CPU cycle of the current counter state
    uint32_t pscUsed_ = 0;    // prescaler actually in effect (buffered)

    // published channel state
    bool outDriven_[4] = {false, false, false, false};
    bool outLevel_[4] = {false, false, false, false};
    bool inLevel_[4] = {false, false, false, false};

    // unsupported-mode reporting (once per timer)
    bool warnedDownCount_ = false;
    bool warnedCenterAlign_ = false;
    bool warnedAdvanced_ = false;

    ChannelOutFn channelOut_;
    std::function<void(int)> irqCallback_;
    std::function<void(const std::string&)> logger_;
    bool traceEvents_ = false;
    bool traceRegs_ = false;
};