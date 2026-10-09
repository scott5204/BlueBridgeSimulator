#include "stm32/usart/Usart.h"

Usart::Usart(uint32_t base, Traits traits) : BusDevice32(base), traits_(traits) {
    reset();
}

void Usart::reset() {
    cr1_ = cr2_ = cr3_ = brr_ = 0;
    gtpr_ = rtor_ = presc_ = 0;
    rdr_ = isr_ = 0;
    oreReadSeq_ = false;
    txHoldValid_ = false;
    txHoldByte_ = 0;
    txShiftActive_ = false;
    txShiftEnd_ = 0;
    txShiftByte_ = 0;
    pinDriven_ = false;
    pinLevel_ = false;
    nowCycles_ = 0;
    clockEnabled_ = false;  // RCC reset clears every gate
}

void Usart::setClockEnabled(bool en) {
    if (en == clockEnabled_) return;
    clockEnabled_ = en;
    if (!en) {
        isr_ = 0;
        dropTransmitter();
        oreReadSeq_ = false;
    }
    pushTxPin(nowCycles_);
}

// ---------------------------------------------------------------------------
// line format / baud decoding (BRR + PRESC + OVER8, RM0440)
// ---------------------------------------------------------------------------
uint32_t Usart::wordBits() const {
    const bool m0 = (cr1_ & kCr1M0) != 0;
    const bool m1 = (cr1_ & kCr1M1) != 0;
    if (m0 && !m1) return 9;   // M0=1 M1=0 -> 9 data bits
    if (!m0 && m1) return 7;   // M0=0 M1=1 -> 7 data bits
    return 8;                  // 00 = 8 bits (11 is the 7-bit + parity code)
}

int Usart::parity() const {
    if (!(cr1_ & kCr1Pce)) return 0;
    return (cr1_ & kCr1Ps) ? 2 : 1;  // 1 = even, 2 = odd
}

uint32_t Usart::stopBitsHalf() const {
    switch ((cr2_ & kCr2StopMask) >> 12) {
    case 0: return 2;   // 1 stop bit
    case 1: return 1;   // 0.5
    case 2: return 4;   // 2
    default: return 3;  // 1.5
    }
}

uint32_t Usart::bitsX2() const {
    const uint32_t bits = 1 + wordBits() + (parity() ? 1u : 0u);  // start+data
    return 2u * bits + stopBitsHalf();
}

// Kernel clock cycles per bit (RM0440): with oversampling by 16
// (OVER8=0) baud = fCK / (PRESC * (16*DIV_Mantissa + DIV_Fraction)); with
// oversampling by 8 (OVER8=1) the factor is 8 and DIV_Fraction has 3 bits.
// The result equals the raw BRR value with PRESC = 1 / OVER8 = 0
// (9600 baud at 80 MHz PCLK2 -> BRR = 0x208D = 8333 cycles per bit).
double Usart::usartDiv() const {
    const uint32_t mant = (brr_ >> 4) & 0xFFFu;
    const bool over8 = (cr1_ & kCr1Over8) != 0;
    const uint32_t frac = over8 ? (brr_ & 0x7u) : (brr_ & 0xFu);
    const uint32_t sub = over8 ? 8u : 16u;
    const double presc = double((presc_ & 0xFu) + 1u);
    return double(mant * sub + frac) * presc;
}

double Usart::baudHz() const {
    if (!clockEnabled_) return 0.0;  // gated kernel clock: no baud at all
    const double div = usartDiv();
    const uint32_t fck = kernelClockHz();
    if (div <= 0.0 || fck == 0) return 0.0;
    return double(fck) / div;
}

uint32_t Usart::baudRate() const {
    const double b = baudHz();
    return b <= 0.0 ? 0u : uint32_t(b + 0.5);
}

uint64_t Usart::bitCycles() const {
    if (!clockEnabled_) return 0;
    const double div = usartDiv();  // kernel clock cycles per bit
    const uint32_t hclk = hclkHz();
    const uint32_t fck = kernelClockHz();
    if (div <= 0.0 || hclk == 0 || fck == 0) return 0;
    const double c = div * double(hclk) / double(fck);
    return c < 1.0 ? 1u : uint64_t(c + 0.5);
}

uint64_t Usart::byteCycles() const {
    const uint64_t bit = bitCycles();
    if (bit == 0) return 0;
    return bit * bitsX2() / 2;
}

// ---------------------------------------------------------------------------
// registers
// ---------------------------------------------------------------------------
uint32_t Usart::readReg(uint32_t regOff) {
    if (!clockEnabled_) return 0;  // unclocked peripheral reads as 0
    switch (regOff) {
    case R_CR1: return cr1_;
    case R_CR2: return cr2_;
    case R_CR3: return cr3_;
    case R_BRR: return brr_;
    case R_GTPR: return gtpr_;
    case R_RTOR: return rtor_;
    case R_RQR: return 0;  // write-only
    case R_ISR: {
        uint32_t v = isr_;
        if (txActive() && txShiftActive_) v |= kIsrBusy;
        if (txActive()) v |= kIsrTeack;
        if (enabled() && rxEnabled()) v |= kIsrReack;
        // RM0440 overrun clearing: a read of ISR while ORE is set, followed by
        // a read of RDR, clears ORE.
        oreReadSeq_ = (isr_ & kIsrOre) != 0;
        return v;
    }
    case R_ICR: return 0;  // write-only
    case R_RDR: {
        const uint32_t v = rdr_;
        isr_ &= ~kIsrRxne;
        if ((isr_ & kIsrOre) && oreReadSeq_) isr_ &= ~kIsrOre;
        oreReadSeq_ = false;
        return v;
    }
    case R_TDR: return 0;  // write-only
    case R_PRESC: return presc_;
    default: return 0;
    }
}

void Usart::writeReg(uint32_t regOff, uint32_t value) {
    if (!clockEnabled_) return;  // writes to an unclocked peripheral are lost
    switch (regOff) {
    case R_CR1: {
        const uint32_t old = cr1_;
        cr1_ = value;
        if (!(value & kCr1Ue)) {
            // USART disabled: transmitter released, flags cleared (RM0440)
            isr_ = 0;
            oreReadSeq_ = false;
            dropTransmitter();
        } else {
            if ((value & kCr1Te) && !(old & kCr1Te))
                setFlag(kIsrTxe, true, kCr1Txeie);  // TDR empty, ready
            if (!(value & kCr1Te)) dropTransmitter();
            // enabling an interrupt whose flag is already set fires at once
            if ((value & kCr1Rxneie) && !(old & kCr1Rxneie) &&
                (isr_ & (kIsrRxne | kIsrOre)))
                fireIrq();
            if ((value & kCr1Tcie) && !(old & kCr1Tcie) && (isr_ & kIsrTc))
                fireIrq();
            if ((value & kCr1Txeie) && !(old & kCr1Txeie) && (isr_ & kIsrTxe))
                fireIrq();
        }
        pushTxPin(nowCycles_);
        break;
    }
    case R_CR2: cr2_ = value; break;
    case R_CR3: cr3_ = value; break;
    case R_BRR: brr_ = value & 0xFFFFu; break;  // takes effect at the next frame
    case R_GTPR: gtpr_ = value & 0xFFFFu; break;
    case R_RTOR: rtor_ = value; break;
    case R_PRESC: presc_ = value & 0xFu; break;
    case R_RQR:
        // RXFRQ: flush RDR / clear RXNE (RM0440). SBKRQ / MMRQ / TXFRQ are
        // accepted without a modelled side effect (no break / mute / DMA).
        if (value & kRqrRxfrq) {
            rdr_ = 0;
            isr_ &= ~kIsrRxne;
        }
        break;
    case R_ICR: {
        if (value & kIcrOrecf) {
            isr_ &= ~kIsrOre;
            oreReadSeq_ = false;
        }
        if (value & kIcrTccf) isr_ &= ~kIsrTc;
        if (value & kIcrIdlecf) isr_ &= ~kIsrIdle;
        isr_ &= ~(value & (kIcrPecf | kIcrFecf | kIcrNecf));  // no error source
        break;
    }
    case R_TDR: {
        // The reference manual: data written while TE=0 is not transmitted.
        if (!txActive()) break;
        isr_ &= ~kIsrTc;  // a new transmission clears TC
        if (!txShiftActive_) {
            startShift(uint8_t(value), nowCycles_);
        } else {
            txHoldValid_ = true;  // TDR occupied: TXE low until the shift ends
            txHoldByte_ = uint8_t(value);
            setFlag(kIsrTxe, false, 0);
        }
        break;
    }
    default: break;
    }
}

// ---------------------------------------------------------------------------
// transmitter (TDR -> shift register -> line, byte level timing)
// ---------------------------------------------------------------------------
void Usart::startShift(uint8_t b, uint64_t startCycle) {
    const uint64_t bc = byteCycles();
    if (bc == 0) return;  // no kernel clock: nothing can move
    txShiftActive_ = true;
    txShiftByte_ = b;
    txShiftEnd_ = startCycle + bc;
    isr_ &= ~kIsrTc;
    pushTxPin(startCycle);  // the line goes low for the frame
}

void Usart::dropTransmitter() {
    txShiftActive_ = false;
    txHoldValid_ = false;
    isr_ &= ~kIsrTxe;
    pushTxPin(nowCycles_);
}

void Usart::advance(uint64_t nowCycles) {
    nowCycles_ = nowCycles;
    if (!clockEnabled_) return;
    int guard = 0;
    while (txShiftActive_ && nowCycles >= txShiftEnd_ && guard++ < 4096) {
        const uint64_t end = txShiftEnd_;
        const uint8_t b = txShiftByte_;
        txShiftActive_ = false;
        // the byte is on the line: hand it to the SoC, which applies the TX
        // pin routing (a wrong AF means the sink is never called)
        if (txSink_) txSink_(b);
        if (txHoldValid_) {
            const uint8_t next = txHoldByte_;
            txHoldValid_ = false;
            startShift(next, end);  // the next frame starts exactly here
            setFlag(kIsrTxe, true, kCr1Txeie);
        } else {
            setFlag(kIsrTc, true, kCr1Tcie);
            setFlag(kIsrTxe, true, kCr1Txeie);
        }
        pushTxPin(end);
    }
}

uint64_t Usart::cyclesToNextEvent() const {
    if (txShiftActive_)
        return (txShiftEnd_ > nowCycles_) ? (txShiftEnd_ - nowCycles_) : 0;
    // No frame in flight -- but an enabled transmitter can start one at ANY
    // moment (the firmware polls TXE and writes TDR). The batch must therefore
    // never exceed one frame time: a byte written inside a long batch is
    // stamped with the batch start, and a batch longer than the frame time
    // would complete it late (and let a second byte slip through in the same
    // batch), which distorts the line timing. 9600 baud (104 us/bit) is far
    // above the batch cap, 115200 is not -- this is what keeps both exact.
    if (clockEnabled_ && txActive()) {
        const uint64_t bc = byteCycles();
        if (bc != 0) return bc;
    }
    return ~0ull;
}

// ---------------------------------------------------------------------------
// receiver
// ---------------------------------------------------------------------------
void Usart::receive(uint8_t b) {
    if (!clockEnabled_ || !enabled() || !rxEnabled()) return;  // dropped
    if (isr_ & kIsrRxne) {
        // overrun: the previous RDR content is kept, this byte is lost
        setFlag(kIsrOre, true, kCr1Rxneie);
        return;
    }
    const uint32_t bits = wordBits();
    const uint32_t mask = (bits == 9) ? 0x1FFu : (bits == 7) ? 0x7Fu : 0xFFu;
    rdr_ = b & mask;
    setFlag(kIsrRxne, true, kCr1Rxneie);
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
void Usart::setFlag(uint32_t flag, bool on, uint32_t enableMask) {
    const bool was = (isr_ & flag) != 0;
    if (on)
        isr_ |= flag;
    else
        isr_ &= ~flag;
    if (!was && on && enableMask != 0 && (cr1_ & enableMask)) fireIrq();
}

void Usart::fireIrq() {
    if (traits_.irq >= 0 && irqCallback_) irqCallback_(traits_.irq);
}

void Usart::pushTxPin(uint64_t cycle) {
    const bool driven = clockEnabled_ && txActive();
    const bool level = !txShiftActive_;  // low while a frame is shifted out
    if (driven == pinDriven_ && level == pinLevel_) return;
    pinDriven_ = driven;
    pinLevel_ = level;
    if (txPin_) txPin_(driven, level, cycle);
}