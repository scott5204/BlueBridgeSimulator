// TIM (stage 3) integration self test: runs the real ARM TIM test firmware on
// the emulated Cortex-M4 and verifies every acceptance test of the timer
// stage through the full hardware chain:
//
//   Test C  base timer   1 kHz update interrupt, real ISR execution
//   Test D  PWM          1000 Hz / 50 % measured at the PA7 pin
//   Test E  dynamic PWM  25 % / 50 % / 75 % + ARR change (2 kHz)
//   Test F  capture      PA15 generator 1 kHz -> 2 kHz, firmware measures both
//                        (plus an overflowing period: 10 Hz > 16-bit counter)
//   Test G  wrong AF     PWM configured but PA7 not in AF -> no pin waveform
//   Test H  CEN = 0      timer configured but never started -> nothing runs
//
// The firmware is driven ONLY through its mailbox command word; every timer
// register is written by the firmware itself. The measurements come from the
// board-level pin monitor (frequency / duty / edges) and from the firmware's
// own capture results -- never from a timer register read by the host.
//
// Exit code 0 = pass.
#include <QCoreApplication>
#include <QElapsedTimer>

#include <cmath>
#include <cstdio>

#include "sim/Simulator.h"

// firmware self-test mailbox, placed at 0x20000000 by the linker script
static constexpr uint32_t kMailBase = 0x20000000u;
static constexpr int kMailWords = 13;
static constexpr uint32_t kMailMagic = 0xC0FFEE03u;

// mailbox indices (firmware/test_tim/main.c)
enum {
    MB_MAGIC = 0,
    MB_CMD = 1,
    MB_PHASE = 2,
    MB_UPD_IRQ = 3,
    MB_CAP_IRQ = 4,
    MB_LAST_CAP = 5,
    MB_DELTA = 6,
    MB_CCR2 = 7,
    MB_CNT3 = 8,
    MB_SR3 = 9,
    MB_CNT2 = 10,
    MB_LOOP = 11,
    MB_OVF = 12,
};

static int g_failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                               \
        }                                                               \
    } while (0)

// percentage tolerance check with a readable log line
static void checkNear(const char* what, double value, double expected,
                      double tolPercent) {
    const double err = std::fabs(value - expected) * 100.0 /
                       (expected != 0.0 ? std::fabs(expected) : 1.0);
    const bool ok = err <= tolPercent;
    std::printf("  %-46s = %10.2f (expected %8.2f, err %5.2f %%) %s\n", what,
                value, expected, err, ok ? "ok" : "FAIL");
    if (!ok) g_failures++;
}

// ---------------------------------------------------------------------------
// host: drives the simulator synchronously (no event loop needed)
// ---------------------------------------------------------------------------
static double dutyPercent(const DigitalSignalMonitor::Stats& w) {
    return w.duty * 100.0;
}

struct Host {
    Simulator sim;
    SimSnapshot last;
    bool faulted = false;
    bool loadFailed = false;
    uint32_t mb[kMailWords] = {};

    Host() {
        QObject::connect(&sim, &Simulator::stateChanged,
                         [this](const SimSnapshot& s) { last = s; });
        QObject::connect(&sim, &Simulator::faulted, [this](const QString& e) {
            faulted = true;
            std::printf("FAULT: %s\n", e.toUtf8().constData());
        });
        QObject::connect(&sim, &Simulator::firmwareLoadFailed,
                         [this](const QString& e) {
                             loadFailed = true;
                             std::printf("load failed: %s\n",
                                         e.toUtf8().constData());
                         });
    }

    void refresh() {
        for (int i = 0; i < kMailWords; i++) {
            uint32_t v = 0;
            sim.debugSramWord(kMailBase + 4u * uint32_t(i), v);
            mb[i] = v;
        }
    }

    bool runToCycles(uint64_t target, qint64 wallMs = 60000) {
        QElapsedTimer w;
        w.start();
        while (!faulted && last.cycles < target && w.elapsed() < wallMs)
            sim.slotStep(400);
        return !faulted;
    }

    // try to reach @wallMs of wall-clock budget while advancing virtual time
    bool runVirtualMs(double ms, qint64 wallMs = 30000) {
        const double hz = last.sysclkHz ? double(last.sysclkHz) : 1.0;
        const uint64_t target = last.cycles + uint64_t(ms * hz / 1000.0);
        return runToCycles(target, wallMs);
    }

    // publish a phase and wait until the firmware mirrors it back
    bool setPhase(uint32_t phase) {
        sim.debugSramWrite(kMailBase + 4u * MB_CMD, phase);
        QElapsedTimer w;
        w.start();
        for (int i = 0; i < 4000 && w.elapsed() < 20000; i++) {
            sim.slotStep(20);
            if (faulted) return false;
            refresh();
            if (mb[MB_PHASE] == phase) return true;
        }
        std::printf("phase %u never applied (mailbox phase=%u)\n", phase,
                    mb[MB_PHASE]);
        return false;
    }

    const DigitalSignalMonitor::Stats pa7() const { return sim.pa7Waveform(); }
};

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    qRegisterMetaType<SimSnapshot>("SimSnapshot");

    if (argc < 2) {
        std::printf("usage: tim_selftest <test_tim.hex>\n");
        return 2;
    }

    Host h;
    h.sim.loadFirmware(QString::fromLocal8Bit(argv[1]));
    CHECK(!h.loadFailed);
    CHECK(h.sim.firmwareLoaded());
    if (g_failures) return 1;

    QElapsedTimer wall;
    wall.start();

    // ---- boot ----
    h.runToCycles(8'000'000);  // ~100 us of firmware execution incl. PLL setup
    h.refresh();
    CHECK(!h.faulted);
    CHECK(h.last.sysclkHz == 80'000'000u);
    CHECK(h.mb[MB_MAGIC] == kMailMagic);
    std::printf("boot: sysclk=%u MHz cycles=%llu phase=%u loop=%u\n",
                h.last.sysclkHz / 1000000u,
                (unsigned long long)h.last.cycles, h.mb[MB_PHASE],
                h.mb[MB_LOOP]);

    std::printf("\n--- Test C: base timer, 1 kHz update interrupt ---\n");
    CHECK(h.setPhase(1));
    CHECK(h.runVirtualMs(200.0));  // settle
    h.refresh();
    const uint64_t c0 = h.last.cycles, u0 = h.mb[MB_UPD_IRQ],
                   e0 = h.last.exceptionReturns;
    const uint32_t cnt0 = h.mb[MB_CNT3];
    CHECK(h.runVirtualMs(500.0));
    h.refresh();
    const uint64_t c1 = h.last.cycles, u1 = h.mb[MB_UPD_IRQ],
                   e1 = h.last.exceptionReturns;
    const double updHz =
        double(u1 - u0) * double(h.last.sysclkHz) / double(c1 - c0);
    std::printf("  updates=%llu exception returns=%llu cnt snapshot 0x%X\n",
                (unsigned long long)(u1 - u0), (unsigned long long)(e1 - e0),
                cnt0);
    checkNear("TIM3 update interrupt rate [Hz]", updHz, 1000.0, 2.0);
    CHECK(u1 > u0);
    CHECK(e1 - e0 > 400);  // real Cortex-M exceptions, ~500 expected

    std::printf("\n--- Test D: PWM at the PA7 pin, 1 kHz 50 %% ---\n");
    CHECK(h.setPhase(2));
    CHECK(h.runVirtualMs(200.0));
    DigitalSignalMonitor::Stats w = h.pa7();
    std::printf("  pin PA7: active=%d edges=%llu freq=%.1f Hz duty=%.1f %%\n",
                int(w.active), (unsigned long long)w.edges, w.frequencyHz,
                dutyPercent(w));
    CHECK(w.active);
    checkNear("PA7 PWM frequency [Hz]", double(w.frequencyHz), 1000.0, 2.0);
    checkNear("PA7 PWM duty [%]", dutyPercent(w), 50.0,
              3.0);
    // the snapshot must expose the same pin-level measurement (GUI path)
    checkNear("snapshot PA7 frequency [Hz]", double(h.last.pa7Frequency), 1000.0,
              2.0);

    std::printf("\n--- Test E: dynamic duty 25/50/75 %% and ARR 2 kHz ---\n");
    CHECK(h.setPhase(3));
    CHECK(h.runVirtualMs(150.0));
    w = h.pa7();
    checkNear("PA7 duty after CCR2=250 [%]",
              dutyPercent(w), 25.0, 4.0);
    CHECK(h.setPhase(2));
    CHECK(h.runVirtualMs(150.0));
    w = h.pa7();
    checkNear("PA7 duty after CCR2=500 [%]",
              dutyPercent(w), 50.0, 4.0);
    CHECK(h.setPhase(4));
    CHECK(h.runVirtualMs(150.0));
    w = h.pa7();
    checkNear("PA7 duty after CCR2=750 [%]",
              dutyPercent(w), 75.0, 4.0);
    CHECK(h.setPhase(8));
    CHECK(h.runVirtualMs(150.0));
    w = h.pa7();
    checkNear("PA7 frequency after ARR=499 [Hz]", double(w.frequencyHz), 2000.0,
              3.0);
    checkNear("PA7 duty after ARR=499 [%]",
              dutyPercent(w), 50.0, 4.0);

    std::printf("\n--- Test F: input capture on PA15 ---\n");
    CHECK(h.setPhase(5));
    h.sim.setPulseFrequency(1000.0);
    h.sim.setPulseEnabled(true);
    CHECK(h.runVirtualMs(300.0));
    h.refresh();
    const uint32_t capIrq0 = h.mb[MB_CAP_IRQ];
    const uint32_t delta1k = h.mb[MB_DELTA];
    const double inHz1k = 1.0e6 / double(delta1k);  // 1 MHz capture counter
    std::printf("  generator 1 kHz: delta=%u counts -> %.1f Hz, captu"
                "res=%u\n",
                delta1k, inHz1k, capIrq0);
    CHECK(capIrq0 > 200);
    checkNear("firmware measured input frequency [Hz]", inHz1k, 1000.0, 3.0);

    h.sim.setPulseFrequency(2000.0);
    CHECK(h.runVirtualMs(200.0));
    h.refresh();
    const uint32_t delta2k = h.mb[MB_DELTA];
    const double inHz2k = 1.0e6 / double(delta2k);
    std::printf("  generator 2 kHz: delta=%u counts -> %.1f Hz\n", delta2k,
                inHz2k);
    checkNear("firmware measured input frequency [Hz]", inHz2k, 2000.0, 4.0);
    checkNear("capture delta ratio (1k/2k)",
              double(delta1k) / double(delta2k), 2.0, 5.0);

    // overflow: 10 Hz at a 1 MHz counter = 100000 counts > 16-bit counter, so
    // the model must really wrap and the firmware must add its overflow count
    h.sim.setPulseFrequency(10.0);
    CHECK(h.runVirtualMs(400.0));
    h.refresh();
    const uint32_t delta10 = h.mb[MB_DELTA];
    const uint32_t ovf = h.mb[MB_OVF];
    std::printf("  generator 10 Hz: delta=%u counts (overflows=%u)\n", delta10,
                ovf);
    CHECK(ovf > 3);  // the real counter wrapped several times
    checkNear("firmware measured 10 Hz period [counts]", double(delta10),
              100000.0, 3.0);
    checkNear("firmware measured input frequency [Hz]", 1.0e6 / double(delta10),
              10.0, 3.0);

    // no generator -> no edges -> no new captures (the capture really comes
    // from the pin waveform, not from a host-side shortcut)
    h.sim.setPulseEnabled(false);
    CHECK(h.runVirtualMs(200.0));
    h.refresh();
    const uint32_t capIrqOff0 = h.mb[MB_CAP_IRQ];
    CHECK(h.runVirtualMs(200.0));
    h.refresh();
    std::printf("  generator off: captures %u -> %u\n", capIrqOff0,
                h.mb[MB_CAP_IRQ]);
    CHECK(h.mb[MB_CAP_IRQ] == capIrqOff0);

    std::printf("\n--- Test G: wrong AF (PWM configured, PA7 not in AF) ---\n");
    CHECK(h.setPhase(6));
    CHECK(h.runVirtualMs(100.0));
    const uint64_t edgesG0 = h.pa7().edges;
    h.refresh();
    const uint32_t updG0 = h.mb[MB_UPD_IRQ];
    const uint32_t cntG0 = h.mb[MB_CNT3];
    CHECK(h.runVirtualMs(100.0));
    h.refresh();
    const DigitalSignalMonitor::Stats wg = h.pa7();
    const uint32_t updG1 = h.mb[MB_UPD_IRQ];
    std::printf("  PA7: active=%d edges %llu -> %llu; timer updates %u -> %u "
                "(CNT sample 0x%X)\n",
                int(wg.active), (unsigned long long)edgesG0,
                (unsigned long long)wg.edges, updG0, updG1, cntG0);
    CHECK(wg.edges == edgesG0);  // not a single pin edge
    CHECK(!wg.active);
    // ... while the timer itself is happily running: the AF layer is what
    // makes the difference, not a stopped timer
    CHECK(updG1 - updG0 > 80);            // ~100 updates in 100 ms
    // The counter itself keeps wrapping at 1 kHz (the update rate above is the
    // proof). Informational only: a single CNT sample is phase aligned with the
    // deterministic main loop, so it is not a meaningful assertion.
    uint32_t cntMoving = 0;
    for (int i = 0; i < 12; i++) {
        CHECK(h.runVirtualMs(5.0));
        h.refresh();
        if (h.mb[MB_CNT3] != 0) cntMoving++;  // 1 MHz counter, sampled blindly
        if (i < 3)
            std::printf("    sample %d: loop=%u CNT3=0x%X SR3=0x%X CNT2=0x%X "
                        "| host CNT3=0x%X SR=0x%X CCR2=0x%X\n",
                        i, h.mb[MB_LOOP], h.mb[MB_CNT3], h.mb[MB_SR3],
                        h.mb[MB_CNT2], h.sim.debugMmioRead(0x40000424u),
                        h.sim.debugMmioRead(0x40000410u),
                        h.sim.debugMmioRead(0x40000438u));
    }
    CHECK(h.mb[MB_CCR2] == 500u);
    std::printf("  internal TIM3 counter still running: %u/12 non-zero samples\n",
                cntMoving);

    std::printf("\n--- Test H: CEN = 0 (timer never started) ---\n");
    CHECK(h.setPhase(7));
    CHECK(h.runVirtualMs(20.0));  // let the single static level settle
    h.refresh();
    const uint32_t updH0 = h.mb[MB_UPD_IRQ];
    const uint32_t cntH0 = h.mb[MB_CNT3];
    const uint64_t edgesH0 = h.pa7().edges;
    CHECK(h.runVirtualMs(100.0));
    h.refresh();
    const DigitalSignalMonitor::Stats wh = h.pa7();
    std::printf("  updates %u -> %u, CNT 0x%X -> 0x%X, edges %llu -> %llu\n",
                updH0, h.mb[MB_UPD_IRQ], cntH0, h.mb[MB_CNT3],
                (unsigned long long)edgesH0,
                (unsigned long long)wh.edges);
    CHECK(h.mb[MB_UPD_IRQ] == updH0);  // no update event at all
    CHECK(h.mb[MB_CNT3] == cntH0);     // counter frozen
    CHECK(wh.edges == edgesH0);        // no pin waveform
    CHECK(!wh.active);

    std::printf("\n--- back to PWM 1 kHz 50 %% (after all the abuse) ---\n");
    CHECK(h.setPhase(2));
    CHECK(h.runVirtualMs(200.0));
    w = h.pa7();
    checkNear("PA7 PWM frequency [Hz]", double(w.frequencyHz), 1000.0, 2.0);
    checkNear("PA7 PWM duty [%]", dutyPercent(w), 50.0,
              3.0);

    std::printf("\n--- Performance: PWM edge density (virtual time per wall second) ---\n");
    // The timer never loops per CPU cycle: PWM edges and interrupts are
    // scheduled as events, so a 20 kHz output must not cost noticeably more
    // than 1 kHz. Measured with the mailbox (MMIO) polling firmware, i.e. a
    // pessimistic workload.
    auto measurePhase = [&](uint32_t phase, const char* what) {
        CHECK(h.setPhase(phase));
        CHECK(h.runVirtualMs(20.0));  // settle
        h.refresh();
        const uint64_t c0 = h.last.cycles;
        QElapsedTimer w;
        w.start();
        CHECK(h.runVirtualMs(100.0));
        const qint64 wallMs = w.elapsed();
        const uint64_t c1 = h.last.cycles;
        const double virtualMs = double(c1 - c0) * 1000.0 / 80.0e6;
        const double speed = virtualMs / double(wallMs);
        const DigitalSignalMonitor::Stats m = h.pa7();
        std::printf("  %-28s freq=%6.0f Hz duty=%5.1f %%  %.0f ms virtual in "
                    "%lld ms wall -> %.2fx real time\n",
                    what, m.frequencyHz, dutyPercent(m),
                    virtualMs, (long long)wallMs, speed);
        CHECK(m.frequencyHz > 0);
        return speed;
    };
    const double sp1k = measurePhase(2, "PWM 1 kHz");
    const double sp10k = measurePhase(9, "PWM 10 kHz");
    const double sp20k = measurePhase(10, "PWM 20 kHz");
    CHECK(sp20k > sp1k * 0.3);  // 20 kHz may not collapse the throughput

    h.refresh();
    std::printf(
        "\ntim_selftest summary: cycles=%llu (%.2f s virtual) wall=%lld ms\n",
        (unsigned long long)h.last.cycles,
        double(h.last.cycles) / 80.0e6, (long long)wall.elapsed());

    if (g_failures == 0) {
        std::printf("tim_selftest: PASS\n");
        return 0;
    }
    std::printf("tim_selftest: %d failure(s)\n", g_failures);
    return 1;
}