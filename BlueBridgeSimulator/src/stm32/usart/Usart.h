#pragma once

#include <cstdint>
#include <functional>

#include "core/IBusDevice.h"

// ============================================================================
// STM32G4 USART model (RM0440 chapter "Universal synchronous asynchronous
// receiver transmitter"), stage 5 -- USART1 of the CT117E-M4.
//
// MMIO base (CMSIS stm32g431xx.h): USART1 0x40013800, USART1_IRQn = 37.
//
// Byte-level timing, event driven: one frame = bit time * bits-per-frame,
// where the bit time is decoded from BRR (PRESC and OVER8 aware) and the
// kernel clock comes from the SoC clock tree -- 8N1 at 9600 baud is 10 bit
// times = 1.0416 ms, so 100 bytes take ~104 ms of VIRTUAL time. There is no
// per-cycle bit clock: a byte is scheduled once and completed exactly at its
// end time.
//
// Implemented (enough for a polling or RXNEIE firmware, HAL or LL style):
//   CR1  UE / RE / TE / RXNEIE / TCIE / TXEIE / PCE+PS+M0/M1 (line format),
//        OVER8
//   CR2  STOP (line format); CR3 / GTPR / RTOR stored
//   BRR  real baud divisor (RM0440): baud = fck / (PRESC * (16*mant + frac))
//        with OVER8=0, the factor 8 and a 3-bit fraction with OVER8=1; the
//        CT117E-M4 default 9600 baud at 80 MHz PCLK2 is BRR = 0x208D
//        (8333 cycles per bit -> 1.0416 ms per 8N1 byte)
//   ISR  RXNE(RXFNE), TC, TXE(TXFNF), ORE; BUSY / TEACK / REACK follow the
//        enabled state. PE/FE/NE are never set (the modelled line is ideal).
//   ICR  ORECF, TCCF, IDLECF, RXFRQ... plus the no-source error flags
//   RDR  reading clears RXNE; the RM0440 overrun sequence (read ISR then read
//        RDR) clears ORE
//   TDR  accepted while TE=1; TXE/TC follow the real TDR -> shift register
//        handshake (TXE returns as soon as the shift register is free, TC only
//        when the last stop bit completed)
//   RQR  RXFRQ flushes RDR / clears RXNE; SBKRQ, MMRQ, TXFRQ accepted
//
// Interrupts: RXNEIE covers RXNE and ORE (non-FIFO mode), TCIE covers TC,
// TXEIE covers TXE; the callback pends USART1_IRQn in the NVIC, exactly like
// the timers and the ADC (single place: Stm32G431::wireUsart()).
//
// Not implemented: parity/framing/noise generation (the line is ideal),
// synchronous & clock modes, LIN / IrDA / SmartCard, half duplex, auto baud
// rate, DMA request generation, FIFO mode (FIFOEN is stored but the register
// set stays in non-FIFO form), wake-up / address filtering, receiver timeout.
// ============================================================================
class Usart : public BusDevice32 {
public:
    struct Traits {
        const char* name;
        int irq;  // NVIC IRQn (USART1 = 37)
    };

    Usart(uint32_t base, Traits traits);
    void reset();

    // RCC APB2 clock gate (APB2ENR.USART1EN). While the gate is closed the
    // register block reads 0 and every write is dropped, like the GPIO ports.
    void setClockEnabled(bool en);
    bool clockEnabled() const { return clockEnabled_; }

    // ---- clock tree inputs (the USART never hard-codes a frequency) ----
    void setClockProviders(std::function<uint32_t()> hclk,
                           std::function<uint32_t()> kernelHz) {
        hclkHz_ = std::move(hclk);
        kernelClockHz_ = std::move(kernelHz);
    }

    // ---- board / SoC wiring ----
    // A byte finished on the line (the SoC applies the TX pin routing: a wrong
    // AF means the sink is never called).
    void setTxSink(std::function<void(uint8_t)> fn) { txSink_ = std::move(fn); }
    // TX pin activity with its exact virtual cycle: driven=false releases the
    // pin (peripheral off), level=false while a frame is being shifted out.
    void setTxPinCallback(
        std::function<void(bool driven, bool level, uint64_t cpuCycle)> fn) {
        txPin_ = std::move(fn);
    }
    void setIrqCallback(std::function<void(int irq)> fn) {
        irqCallback_ = std::move(fn);
    }
    // A byte arrived from the line. The SoC calls this only when the firmware
    // really routed the RX pin to this USART (AF7), so a wrong RX AF really
    // leaves RXNE at 0.
    void receive(uint8_t b);

    // ---- virtual time ----
    // Register-driven side effects inside a batch are stamped with the batch
    // start (same convention as the GPIO ports).
    void setNowCycles(uint64_t cycles) { nowCycles_ = cycles; }
    void advance(uint64_t nowCycles);
    // HCLK cycles until the next transmitter event (~0ull = none)
    uint64_t cyclesToNextEvent() const;
    // Re-apply the pin drive (MODER/AFR changed: a pin switched to AF7 must
    // show the transmitter idle level immediately)
    void refreshPins() { pushTxPin(nowCycles_); }

    // ---- line format / timing (GUI "Status / Baud / Format" rows) ----
    uint32_t baudRate() const;  // decoded from BRR/PRESC/OVER8, rounded
    uint32_t wordBits() const;  // 7 / 8 / 9 data bits
    int parity() const;         // 0 = none, 1 = even, 2 = odd
    uint32_t stopBitsHalf() const;  // 2 = 1 stop bit, 1 = 0.5, 4 = 2, 3 = 1.5
    uint64_t bitCycles() const;     // HCLK cycles per bit (0 = no clock)
    uint64_t byteCycles() const;    // one full frame

    // ---- inspection ----
    const Traits& traits() const { return traits_; }
    bool enabled() const { return (cr1_ & kCr1Ue) != 0; }
    bool txEnabled() const { return (cr1_ & kCr1Te) != 0; }
    bool rxEnabled() const { return (cr1_ & kCr1Re) != 0; }
    uint32_t isr() const { return isr_; }
    bool rxne() const { return (isr_ & kIsrRxne) != 0; }
    bool txe() const { return (isr_ & kIsrTxe) != 0; }
    bool tc() const { return (isr_ & kIsrTc) != 0; }
    bool ore() const { return (isr_ & kIsrOre) != 0; }
    uint32_t brr() const { return brr_; }
    uint32_t presc() const { return presc_ & 0xFu; }
    bool txBusy() const { return txShiftActive_; }
    uint32_t lastRxByte() const { return rdr_; }

    // ---- register offsets (USART_TypeDef, CMSIS) ----
    static constexpr uint32_t R_CR1 = 0x00, R_CR2 = 0x04, R_CR3 = 0x08,
                             R_BRR = 0x0C, R_GTPR = 0x10, R_RTOR = 0x14,
                             R_RQR = 0x18, R_ISR = 0x1C, R_ICR = 0x20,
                             R_RDR = 0x24, R_TDR = 0x28, R_PRESC = 0x2C;
    // ---- bit definitions (verified against docs/stm32g431xx.h) ----
    static constexpr uint32_t kCr1Ue = 1u << 0, kCr1Re = 1u << 2,
                             kCr1Te = 1u << 3, kCr1Rxneie = 1u << 5,
                             kCr1Tcie = 1u << 6, kCr1Txeie = 1u << 7,
                             kCr1Ps = 1u << 9, kCr1Pce = 1u << 10,
                             kCr1M0 = 1u << 12, kCr1Over8 = 1u << 15,
                             kCr1M1 = 1u << 28;
    static constexpr uint32_t kIsrPe = 1u << 0, kIsrFe = 1u << 1,
                             kIsrNe = 1u << 2, kIsrOre = 1u << 3,
                             kIsrIdle = 1u << 4, kIsrRxne = 1u << 5,
                             kIsrTc = 1u << 6, kIsrTxe = 1u << 7,
                             kIsrBusy = 1u << 16, kIsrTeack = 1u << 21,
                             kIsrReack = 1u << 22;
    static constexpr uint32_t kIcrPecf = 1u << 0, kIcrFecf = 1u << 1,
                             kIcrNecf = 1u << 2, kIcrOrecf = 1u << 3,
                             kIcrIdlecf = 1u << 4, kIcrTccf = 1u << 6;
    static constexpr uint32_t kRqrSbkrq = 1u << 1, kRqrRxfrq = 1u << 3;
    static constexpr uint32_t kCr2StopMask = 0x3u << 12;

private:
    uint32_t readReg(uint32_t regOff) override;
    void writeReg(uint32_t regOff, uint32_t value) override;

    // decoded divisor, in kernel clock cycles per bit (PRESC included)
    double usartDiv() const;
    double baudHz() const;
    uint32_t bitsX2() const;  // 2 * (start + data + parity + stop)
    uint32_t kernelClockHz() const {
        return kernelClockHz_ ? kernelClockHz_() : 0;
    }
    uint32_t hclkHz() const { return hclkHz_ ? hclkHz_() : 0; }

    bool txActive() const { return enabled() && txEnabled(); }
    void startShift(uint8_t b, uint64_t startCycle);
    void dropTransmitter();
    // flag helper: (re)set a status bit and raise the interrupt if its enable
    // bit is set and the flag went 0 -> 1
    void setFlag(uint32_t flag, bool on, uint32_t enableMask);
    void fireIrq();
    void pushTxPin(uint64_t cycle);

    Traits traits_;
    bool clockEnabled_ = false;

    uint32_t cr1_ = 0, cr2_ = 0, cr3_ = 0, brr_ = 0;
    uint32_t gtpr_ = 0, rtor_ = 0, presc_ = 0;
    uint32_t rdr_ = 0, isr_ = 0;
    // RM0440 overrun clearing: read ISR (ORE set) then read RDR
    bool oreReadSeq_ = false;

    // transmitter: TDR (hold) -> shift register -> line, byte level
    bool txHoldValid_ = false;
    uint8_t txHoldByte_ = 0;
    bool txShiftActive_ = false;
    uint64_t txShiftEnd_ = 0;
    uint8_t txShiftByte_ = 0;

    // last pin drive pushed to the SoC (so only real changes are reported)
    bool pinDriven_ = false;
    bool pinLevel_ = false;

    uint64_t nowCycles_ = 0;
    std::function<uint32_t()> hclkHz_;
    std::function<uint32_t()> kernelClockHz_;
    std::function<void(uint8_t)> txSink_;
    std::function<void(bool, bool, uint64_t)> txPin_;
    std::function<void(int)> irqCallback_;
};