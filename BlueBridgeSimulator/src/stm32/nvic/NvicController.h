#pragma once

#include <cstdint>

// ============================================================================
// ARMv7-M NVIC (nested vectored interrupt controller) state + SCB exception
// state. The bus-facing registers are handled by Ppb; the Simulator uses
// this class to decide when to inject exceptions and at which priority.
//
// STM32G431: 102 external IRQs (NVIC_ISER0..2 used), 4 implemented priority
// bits (16 levels).
// ============================================================================
class NvicController {
public:
    static constexpr int kMaxExternalIrq = 102;
    static constexpr int kIrqWords = 4;         // ISER0..3
    static constexpr int kPriorityBits = 4;     // STM32G4 implements 4 bits

    void reset() {
        for (auto& w : iser_) w = 0;
        for (auto& w : ispr_) w = 0;
        for (auto& b : ipr_) b = 0;
        for (auto& b : shpr_) b = 0;
        sysPend_ = 0;  // bit14 = PendSV, bit15 = SysTick
        activeException_ = 0;
    }

    // ---- enables ----
    bool irqEnabled(int irq) const {
        if (irq < 0 || irq >= kMaxExternalIrq) return false;
        return (iser_[irq / 32] >> (irq % 32)) & 1u;
    }
    uint32_t iser(int i) const { return iser_[i]; }
    void iserWrite(int i, uint32_t v) {
        if (i < kIrqWords) iser_[i] |= v;
    }
    void icerWrite(int i, uint32_t v) {
        if (i < kIrqWords) iser_[i] &= ~v;
    }

    // ---- pending ----
    void pend(int exceptionNumber) {
        if (exceptionNumber >= 16 && exceptionNumber < 16 + kMaxExternalIrq)
            ispr_[(exceptionNumber - 16) / 32] |=
                1u << ((exceptionNumber - 16) % 32);
        else if (exceptionNumber == 14 || exceptionNumber == 15)
            sysPend_ |= 1u << exceptionNumber;
    }
    void clearPend(int exceptionNumber) {
        if (exceptionNumber >= 16 && exceptionNumber < 16 + kMaxExternalIrq)
            ispr_[(exceptionNumber - 16) / 32] &=
                ~(1u << ((exceptionNumber - 16) % 32));
        else if (exceptionNumber == 14 || exceptionNumber == 15)
            sysPend_ &= ~(1u << exceptionNumber);
    }
    bool isPending(int exceptionNumber) const {
        if (exceptionNumber >= 16 && exceptionNumber < 16 + kMaxExternalIrq)
            return (ispr_[(exceptionNumber - 16) / 32] >>
                    ((exceptionNumber - 16) % 32)) &
                   1u;
        if (exceptionNumber == 14 || exceptionNumber == 15)
            return (sysPend_ >> exceptionNumber) & 1u;
        return false;
    }
    uint32_t ispr(int i) const { return ispr_[i]; }
    void isprWrite(int i, uint32_t v) {
        if (i < kIrqWords) ispr_[i] |= v;
    }
    void icprWrite(int i, uint32_t v) {
        if (i < kIrqWords) ispr_[i] &= ~v;
    }

    // ---- priorities ----
    // exceptionNumber: 16+ = external IRQ, 4..15 = system exceptions
    void setPriorityByte(int exceptionNumber, uint8_t prio) {
        if (exceptionNumber >= 16 && exceptionNumber < 16 + kMaxExternalIrq)
            ipr_[exceptionNumber - 16] = prio;
        else if (exceptionNumber >= 4 && exceptionNumber <= 15)
            shpr_[exceptionNumber - 4] = prio;
    }
    uint8_t priorityByte(int exceptionNumber) const {
        if (exceptionNumber >= 16 && exceptionNumber < 16 + kMaxExternalIrq)
            return ipr_[exceptionNumber - 16];
        if (exceptionNumber >= 4 && exceptionNumber <= 15)
            return shpr_[exceptionNumber - 4];
        return 0;
    }
    // implemented (hardware) priority: only the top 4 bits matter
    int priority(int exceptionNumber) const {
        return priorityByte(exceptionNumber) >> (8 - kPriorityBits);
    }
    uint8_t iprByte(int irq) const { return ipr_[irq]; }
    uint8_t shprByte(int sysException) const {
        return shpr_[sysException - 4];
    }

    // ---- active exception tracking ----
    int activeException() const { return activeException_; }
    void setActiveException(int n) { activeException_ = n; }

    // Highest (numerically lowest priority value) pending+enabled external
    // IRQ, or 0. System exceptions (SysTick/PendSV) are handled by the
    // caller. Used for ICSR.VectPending and interrupt selection.
    int highestPendingEnabledExternal() const {
        int best = 0;
        int bestPrio = 0x10000;
        for (int irq = 0; irq < kMaxExternalIrq; irq++) {
            if (!isPending(irq + 16) || !irqEnabled(irq)) continue;
            int p = priority(irq + 16);
            if (p < bestPrio) {
                bestPrio = p;
                best = irq + 16;
            }
        }
        return best;
    }

private:
    uint32_t iser_[kIrqWords] = {0};
    uint32_t ispr_[kIrqWords] = {0};
    uint8_t ipr_[kMaxExternalIrq] = {0};
    uint8_t shpr_[12] = {0};  // exceptions 4..15
    uint32_t sysPend_ = 0;
    int activeException_ = 0;
};
