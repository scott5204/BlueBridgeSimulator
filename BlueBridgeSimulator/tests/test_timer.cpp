// Timer peripheral unit test (no CPU / firmware involved): register file
// behaviour, counting, update events, PWM output levels, input capture.
//
// The timer is driven directly at register level, exactly like firmware would
// through MMIO; clocking uses explicit HCLK / timer-clock values so the
// expected numbers are exact.
#include <cstdio>
#include <vector>

#include "stm32/tim/Timer.h"

static int g_failures = 0;
#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            g_failures++;                                                \
        }                                                                \
    } while (0)
#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        const auto va = (a);                                                  \
        const auto vb = (b);                                                  \
        if (va != vb) {                                                       \
            std::printf("FAIL %s:%d: %s == %s (%llu != %llu)\n", __FILE__,    \
                        __LINE__, #a, #b, (unsigned long long)va,             \
                        (unsigned long long)vb);                              \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

static constexpr uint32_t HCLK = 80'000'000u;  // 80 MHz, timer clock == HCLK

namespace {
struct Edge {
    int ch;
    bool driven;
    bool level;
    uint64_t cycle;
};

struct Fixture {
    Timer tim;
    std::vector<Edge> edges;
    int irqCount = 0;
    uint64_t now = 0;

    Fixture()
        : tim(0x40000400u, {"TIM3", 4, /*advanced=*/false, /*bit32=*/false,
                            /*irq=*/29, /*apb=*/1}) {
        tim.setChannelOutputCallback(
            [this](int ch, bool driven, bool level, uint64_t cycle) {
                edges.push_back(Edge{ch, driven, level, cycle});
            });
        tim.setIrqCallback([this](int irq) {
            (void)irq;
            irqCount++;
        });
    }

    // advance @cpu cycles (timer clock == HCLK in this test)
    void run(uint64_t cpu) {
        now += cpu;
        tim.advance(now, cpu, HCLK, HCLK);
    }
    uint32_t rd(uint32_t off) { return tim.busRead(tim.base() + off, 4); }
    void wr(uint32_t off, uint32_t v) {
        tim.busWrite(tim.base() + off, v, 4);
    }
};
}  // namespace

int main() {
    // ---- reset values (RM0440) ----
    {
        Fixture f;
        CHECK_EQ(f.rd(Timer::R_CR1), 0u);
        CHECK_EQ(f.rd(Timer::R_CNT), 0u);
        CHECK_EQ(f.rd(Timer::R_PSC), 0u);
        CHECK_EQ(f.rd(Timer::R_ARR), 0xFFFFu);
        CHECK_EQ(f.rd(Timer::R_SR), 0u);
        CHECK_EQ(f.rd(Timer::R_CCER), 0u);
        CHECK_EQ(f.rd(Timer::R_BDTR), 0u);
    }

    // ---- register read/write ----
    {
        Fixture f;
        f.wr(Timer::R_PSC, 79);
        CHECK_EQ(f.rd(Timer::R_PSC), 79u);
        f.wr(Timer::R_ARR, 999);
        CHECK_EQ(f.rd(Timer::R_ARR), 999u);
        f.wr(Timer::R_CNT, 12345);
        CHECK_EQ(f.rd(Timer::R_CNT), 12345u);
        f.wr(Timer::R_CCR1 + 4, 400);  // CCR2
        CHECK_EQ(f.rd(Timer::R_CCR1 + 4), 400u);
        f.wr(Timer::R_DIER, 0x0001u);
        CHECK_EQ(f.rd(Timer::R_DIER), 1u);
        // EGR is write-only
        f.wr(Timer::R_EGR, 0x1u);
        CHECK_EQ(f.rd(Timer::R_EGR), 0u);
    }

    // ---- CEN = 0: the counter must not run (stage-3 acceptance Test H) ----
    {
        Fixture f;
        f.wr(Timer::R_PSC, 0);
        f.wr(Timer::R_ARR, 999);
        f.run(10'000);
        CHECK_EQ(f.rd(Timer::R_CNT), 0u);
        CHECK_EQ(f.tim.status() & 1u, 0u);  // no UIF either
    }

    // ---- counting with PSC, update event, UIF and interrupt ----
    {
        Fixture f;
        f.wr(Timer::R_PSC, 79);   // 80 MHz / 80 = 1 MHz counter clock
        f.wr(Timer::R_ARR, 999);  // 1 kHz period
        f.wr(Timer::R_DIER, 0x0001u);  // UIE
        f.wr(Timer::R_CR1, 0x1u);      // CEN
        f.run(500 * 80);               // 500 counter ticks
        CHECK_EQ(f.rd(Timer::R_CNT), 500u);
        CHECK_EQ(f.irqCount, 0);
        f.run(600 * 80);  // crosses ARR at tick 999 -> one update event
        CHECK_EQ(f.rd(Timer::R_CNT), 100u);
        CHECK_EQ(f.irqCount, 1);
        CHECK((f.rd(Timer::R_SR) & 1u) != 0);  // UIF

        // SR: writing 1 keeps a flag, writing 0 clears it (RM0440 / HAL)
        f.wr(Timer::R_SR, 0xFFFFFFFFu);
        CHECK((f.rd(Timer::R_SR) & 1u) != 0);
        f.wr(Timer::R_SR, 0xFFFFFFFEu);
        CHECK_EQ(f.rd(Timer::R_SR) & 1u, 0u);

        // 1000 more counter ticks -> exactly one more interrupt
        f.run(1000 * 80);
        CHECK_EQ(f.irqCount, 2);
    }

    // ---- EGR.UG reloads the prescaler and the counter, sets UIF ----
    {
        Fixture f;
        f.wr(Timer::R_ARR, 999);
        f.wr(Timer::R_DIER, 0x0001u);
        f.wr(Timer::R_PSC, 79);
        f.wr(Timer::R_CR1, 0x1u);
        f.run(10'000);
        CHECK(f.rd(Timer::R_CNT) > 0);
        const int irqBefore = f.irqCount;
        f.wr(Timer::R_EGR, 0x0001u);  // UG
        CHECK_EQ(f.rd(Timer::R_CNT), 0u);
        CHECK((f.rd(Timer::R_SR) & 1u) != 0);
        CHECK_EQ(f.irqCount, irqBefore + 1);
        // URS = 1: UG must not set UIF (RM0440 TIMx_SR)
        f.wr(Timer::R_SR, ~1u);
        f.wr(Timer::R_CR1, 0x1u | 0x4u);  // CEN | URS
        f.wr(Timer::R_EGR, 0x0001u);
        CHECK_EQ(f.rd(Timer::R_SR) & 1u, 0u);
    }

    // ---- PWM mode 1: pin follows CNT < CCR (channel 2, PA7 usage) ----
    {
        Fixture f;
        f.wr(Timer::R_PSC, 79);
        f.wr(Timer::R_ARR, 999);
        f.wr(Timer::R_CCR1 + 4, 400);  // CCR2 = 400 -> 40 % duty
        // CCMR1: CC2S = 00 (bits 9:8, output), OC2M = 110 (bits 14:12, PWM1)
        f.wr(Timer::R_CCMR1, 0x6000u);
        f.wr(Timer::R_CCER, 0x0010u);  // CC2E
        f.wr(Timer::R_CR1, 0x1u);
        f.edges.clear();  // drop the initial drive published by the CCER write
        f.run(1000 * 80);  // one full period (1 counter tick = 80 CPU cycles)
        // channel 2 goes low at CNT = CCR = 400 and high again at the update
        CHECK_EQ(f.edges.size(), 2u);
        CHECK_EQ(f.edges[0].ch, 1);
        CHECK_EQ(f.edges[0].level, false);
        CHECK_EQ(f.edges[0].cycle, 400 * 80u);  // high time = 400 us of 1000 us
        CHECK_EQ(f.edges[1].level, true);
        CHECK_EQ(f.edges[1].cycle, 1000 * 80u);  // low time = 600 us

        // polarity: CC2P = 1 inverts the pin (still driven)
        f.wr(Timer::R_CCER, 0x0030u);
        CHECK(f.tim.outputDriven(1));
        CHECK(!f.tim.outputLevel(1));  // CNT=0 < 400 -> OCxREF high -> pin low

        // CC2E = 0 releases the pin
        f.wr(Timer::R_CCER, 0x0020u);
        CHECK(!f.tim.outputDriven(1));
    }

    // ---- ARPE: buffered ARR takes effect on the update event ----
    {
        Fixture f;
        f.wr(Timer::R_PSC, 79);   // 1 counter tick = 80 CPU cycles
        f.wr(Timer::R_ARR, 999);
        f.wr(Timer::R_CR1, 0x1u | 0x80u);  // CEN | ARPE
        f.run(100 * 80);
        CHECK_EQ(f.rd(Timer::R_CNT), 100u);
        f.wr(Timer::R_ARR, 499);           // buffered until the update event
        f.run(900 * 80);                   // crosses the update at 999
        CHECK_EQ(f.rd(Timer::R_ARR), 499u);
        CHECK_EQ(f.rd(Timer::R_CNT), 0u);
        f.run(500 * 80);                   // one new (shorter) period
        CHECK_EQ(f.rd(Timer::R_CNT), 0u);  // with ARR=999 it would be 500
    }

    // ---- MOE gate (advanced timers: TIM15/16/17 must not drive before MOE) ----
    {
        Timer tim(0x40014400u, {"TIM16", 1, /*advanced=*/true, false, 25, 2});
        int drivenCount = 0;
        bool driven = false;
        tim.setChannelOutputCallback(
            [&](int, bool d, bool level, uint64_t) {
                driven = d;
                if (d) drivenCount++;
                (void)level;
            });
        tim.busWrite(tim.base() + Timer::R_ARR, 999, 4);
        tim.busWrite(tim.base() + Timer::R_CCR1, 500, 4);
        tim.busWrite(tim.base() + Timer::R_CCMR1, 0x0060u, 4);
        tim.busWrite(tim.base() + Timer::R_CCER, 0x0001u, 4);
        tim.busWrite(tim.base() + Timer::R_CR1, 0x1u, 4);
        uint64_t now = 0;
        now += 1000;
        tim.advance(now, 1000, HCLK, HCLK);
        CHECK(!driven);  // MOE = 0 -> pin released
        tim.busWrite(tim.base() + Timer::R_BDTR, 0x8000u, 4);  // MOE = 1
        CHECK(driven);
        CHECK(drivenCount > 0);
    }

    // ---- input capture: edge latches CNT into CCRx, sets CCxIF + IRQ ----
    {
        Fixture f;
        f.wr(Timer::R_ARR, 0xFFFFu);
        f.wr(Timer::R_CCMR1, 0x0100u);  // CC2S = 01 -> input (TI2)
        f.wr(Timer::R_CCER, 0x0010u);   // CC2E, CC2P = 0 (rising edge)
        f.wr(Timer::R_DIER, 0x0004u);   // CC2IE
        f.wr(Timer::R_CR1, 0x1u);
        f.run(12345 * 1);  // 1 timer clock per CPU cycle in this block? no:
        // in this fixture PSC = 0 and timer clock == HCLK, so CNT == cycles
        CHECK_EQ(f.rd(Timer::R_CNT), 12345u);

        const int irqBefore = f.irqCount;
        f.tim.onChannelPinLevel(1, true, f.now);   // rising edge
        CHECK_EQ(f.rd(Timer::R_CCR1 + 4), 12345u);  // captured value
        CHECK((f.rd(Timer::R_SR) & 0x4u) != 0);     // CC2IF
        CHECK_EQ(f.irqCount, irqBefore + 1);

        // falling edge must not capture while only rising is selected
        f.wr(Timer::R_SR, ~0x4u);
        f.run(100);
        f.tim.onChannelPinLevel(1, false, f.now);
        CHECK_EQ(f.rd(Timer::R_CCR1 + 4), 12345u);
        CHECK_EQ(f.rd(Timer::R_SR) & 0x4u, 0u);

        // CC2P = 1 -> falling edge captures
        f.wr(Timer::R_CCER, 0x0030u);
        f.tim.onChannelPinLevel(1, false, f.now);
        CHECK_EQ(f.rd(Timer::R_CCR1 + 4), 12445u);
        CHECK((f.rd(Timer::R_SR) & 0x4u) != 0);

        // CCxE = 0 -> capture disabled
        f.wr(Timer::R_SR, ~0x4u);
        f.wr(Timer::R_CCER, 0x0030u & ~0x10u);
        f.run(10);
        f.tim.onChannelPinLevel(1, false, f.now);
        CHECK_EQ(f.rd(Timer::R_SR) & 0x4u, 0u);
    }

    // ---- overflow-agnostic capture (firmware handles the wrap itself) ----
    {
        Fixture f;
        f.wr(Timer::R_ARR, 0xFFFFu);
        f.wr(Timer::R_CCMR1, 0x0100u);
        f.wr(Timer::R_CCER, 0x0010u);
        f.wr(Timer::R_CR1, 0x1u);
        f.run(0xFF00);
        f.tim.onChannelPinLevel(1, true, f.now);
        const uint32_t first = f.rd(Timer::R_CCR1 + 4);
        f.run(0x200);  // wraps past the ARR
        f.tim.onChannelPinLevel(1, true, f.now);
        const uint32_t second = f.rd(Timer::R_CCR1 + 4);
        CHECK_EQ(first, 0xFF00u);
        CHECK_EQ(second, 0x0100u);  // raw counter, no auto-correction
    }

    if (g_failures == 0) {
        std::printf("test_timer: all tests passed\n");
        return 0;
    }
    std::printf("test_timer: %d failure(s)\n", g_failures);
    return 1;
}