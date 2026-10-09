#pragma once

#include <cstdint>
#include <functional>

// ============================================================================
// ARM Cortex-M SysTick timer (24-bit down counter).
//
// Registers live on the private peripheral bus and are handled by Ppb:
//   SYST_CSR  0xE000E010
//   SYST_RVR  0xE000E014
//   SYST_CVR  0xE000E018
//   SYST_CALIB 0xE000E01C
//
// The virtual clock advances the counter; reaching zero sets COUNTFLAG and
// (with TICKINT) pends the SysTick exception (IRQ 15).
// ============================================================================
class SysTickTimer {
public:
    static constexpr uint32_t ENABLE = 1u << 0;
    static constexpr uint32_t TICKINT = 1u << 1;
    static constexpr uint32_t CLKSOURCE = 1u << 2;
    static constexpr uint32_t COUNTFLAG = 1u << 16;

    void reset() {
        csr_ = 0;
        rvr_ = 0;
        cvr_ = 0;
    }

    uint32_t csr() const { return csr_; }
    uint32_t rvr() const { return rvr_; }
    uint32_t cvr() const { return cvr_; }

    void writeCsr(uint32_t v) {
        uint32_t keep = csr_ & COUNTFLAG;  // COUNTFLAG only cleared by read
        csr_ = (v & 0x7u) | keep;
    }
    void writeRvr(uint32_t v) { rvr_ = v & 0x00FFFFFFu; }
    void writeCvr(uint32_t) {
        // any write clears the counter to zero and clears COUNTFLAG; the
        // counter reloads from RVR on the next tick (no spurious underflow)
        cvr_ = rvr_;
        csr_ &= ~COUNTFLAG;
    }
    uint32_t readCsr() {
        uint32_t v = csr_;
        csr_ &= ~COUNTFLAG;  // reading clears COUNTFLAG
        return v;
    }
    uint32_t readCvr() const { return cvr_; }

    // Advance the counter by @ticks timer clocks. Calls @pend once per
    // underflow (period) when TICKINT is enabled.
    void advance(uint64_t ticks, const std::function<void()>& pend) {
        if (!(csr_ & ENABLE)) return;
        // Independent guard: the number of underflows this delta can possibly
        // contain is bounded, so a corrupt delta can never turn this into an
        // endless loop (that is exactly how the GUI froze on 2026-09-26).
        uint64_t guard = ticks / (uint64_t(rvr_) + 1) + 2;
        while (ticks > 0 && guard-- > 0) {
            if (cvr_ > ticks) {
                cvr_ -= uint32_t(ticks);
                return;
            }
            ticks -= uint64_t(cvr_) + 1;
            cvr_ = rvr_;
            csr_ |= COUNTFLAG;
            if (csr_ & TICKINT) pend();
            if (rvr_ == 0) break;
        }
    }

    // Timer clocks until the next underflow (for batch budgeting).
    uint64_t cyclesToEvent() const {
        if (!(csr_ & ENABLE) || rvr_ == 0) return ~0ull;
        return uint64_t(cvr_) + 1;
    }

private:
    uint32_t csr_ = 0;
    uint32_t rvr_ = 0;
    uint32_t cvr_ = 0;
};
