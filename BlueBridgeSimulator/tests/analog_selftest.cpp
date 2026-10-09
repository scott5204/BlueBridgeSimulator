// Stage-4 headless acceptance test: PA15 pulse input, R37/R38 ADC inputs and
// the PA7 PWM output measurement, plus the blue-bridge style closed loop.
//
// Everything is driven exactly like the GUI does it:
//   * the host configures BOARD sources only (pulse frequency, R37/R38 volts)
//   * the firmware configures every TIM/ADC register itself
//   * PA15 is measured through the firmware's own TIM2 capture
//   * PA7 is measured from the FINAL pin waveform (DigitalSignalMonitor),
//     never from PSC/ARR/CCR
//
// Test 1  PA15 pulse input, firmware capture: 400 / 1000 / 5000 / 20000 Hz
// Test 2  R37 / R38 ADC codes at 0 V / 1.65 V / 3.3 V
// Test 3  PA7 PWM measured at the pin: 1000 Hz 50 % and 2000 Hz 25 %
// Test 4  wrong AF: PWM configured, PA7 not routed -> 0 Hz / INACTIVE
// Test 5  closed loop: R37 -> duty, R38 -> frequency, PA15 vs PA7 -> ALARM
//
// Exit code 0 = pass.
#include <QCoreApplication>
#include <QElapsedTimer>

#include <cmath>
#include <cstdio>

#include "sim/Simulator.h"

// firmware mailbox, placed at 0x20000000 by the linker script
static constexpr uint32_t kMailBase = 0x20000000u;
static constexpr int kMailWords = 17;
static constexpr uint32_t kMailMagic = 0xC0FFEE04u;

enum {
    MB_MAGIC = 0,
    MB_CMD = 1,
    MB_PHASE = 2,
    MB_ADC_R37 = 3,
    MB_ADC_R38 = 4,
    MB_MV_R37 = 5,
    MB_MV_R38 = 6,
    MB_PA15_HZ = 7,
    MB_PA7_HZ = 8,
    MB_PA7_DUTY = 9,
    MB_ALARM = 10,
    MB_DELTA = 11,
    MB_OVF = 12,
    MB_LOOP = 13,
    MB_PB4_HZ = 14,
    MB_PB4_DELTA = 15,
    MB_PB4_OVF = 16,
};

static int g_failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                               \
        }                                                               \
    } while (0)

static void checkNear(const char* what, double value, double expected,
                      double tolPercent) {
    const double err = std::fabs(value - expected) * 100.0 /
                       (expected != 0.0 ? std::fabs(expected) : 1.0);
    const bool ok = err <= tolPercent;
    std::printf("  %-44s = %10.2f (expected %8.2f, err %6.2f %%) %s\n", what,
                value, expected, err, ok ? "ok" : "FAIL");
    if (!ok) g_failures++;
}

static uint32_t ledMask(const SimSnapshot& s) {
    uint32_t m = 0;
    for (int i = 0; i < 8; i++)
        if (s.leds[i]) m |= 1u << i;
    return m;
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

    bool runToCycles(uint64_t target, qint64 wallMs = 20000) {
        QElapsedTimer w;
        w.start();
        while (!faulted && last.cycles < target && w.elapsed() < wallMs)
            sim.slotStep(200);
        if (last.cycles < target)
            std::printf("  [warn] runToCycles: %.0f ms virtual not reached "
                        "(wall budget %lld ms, cycles=%llu)\n",
                        double(target - last.cycles) * 1000.0 / 80.0e6,
                        (long long)wallMs, (unsigned long long)last.cycles);
        return !faulted;
    }

    bool runVirtualMs(double ms, qint64 wallMs = 20000) {
        const double hz = last.sysclkHz ? double(last.sysclkHz) : 1.0;
        return runToCycles(last.cycles + uint64_t(ms * hz / 1000.0), wallMs);
    }

    bool setPhase(uint32_t phase) {
        sim.debugSramWrite(kMailBase + 4u * MB_CMD, phase);
        QElapsedTimer w;
        w.start();
        for (int i = 0; i < 600 && w.elapsed() < 8000; i++) {
            sim.slotStep(20);
            if (faulted) return false;
            refresh();
            if (mb[MB_PHASE] == phase) return true;
        }
        std::printf("phase %u never applied (mailbox phase=%u)\n", phase,
                    mb[MB_PHASE]);
        return false;
    }

    // configure the PA15 board source the way the GUI knob does
    void setPulse(double hz, bool on = true) {
        sim.setPulseFrequency(hz);
        sim.setPulseEnabled(on);
    }

    // configure the PB4 board source (second pulse input)
    void setPb4Pulse(double hz, bool on = true) {
        sim.setPb4PulseFrequency(hz);
        sim.setPb4PulseEnabled(on);
    }

    const DigitalSignalMonitor::Stats pa7() const { return sim.pa7Waveform(); }
};

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    qRegisterMetaType<SimSnapshot>("SimSnapshot");

    if (argc < 2) {
        std::printf("usage: analog_selftest <test_adc_pwm.hex>\n");
        return 2;
    }

    Host h;
    h.sim.loadFirmware(QString::fromLocal8Bit(argv[1]));
    CHECK(!h.loadFailed);
    CHECK(h.sim.firmwareLoaded());
    if (g_failures) return 1;

    QElapsedTimer wall;
    wall.start();

    // ---- boot: wait until the LCD bring-up finished and the loop is running ----
    h.runToCycles(8'000'000);
    for (int i = 0; i < 40; i++) {
        h.refresh();
        if (h.mb[MB_LOOP] > 0 && h.last.lcdPixelWrites > 1000) break;
        CHECK(h.runVirtualMs(100.0, 6000));
    }
    h.refresh();
    CHECK(!h.faulted);
    CHECK(h.last.sysclkHz == 80'000'000u);
    CHECK(h.mb[MB_MAGIC] == kMailMagic);
    std::printf("boot: sysclk=%u MHz cycles=%llu loop=%u\n",
                h.last.sysclkHz / 1000000u,
                (unsigned long long)h.last.cycles, h.mb[MB_LOOP]);
    std::printf("panel: lcdPixels=%llu cmds=%llu\n",
                (unsigned long long)h.last.lcdPixelWrites,
                (unsigned long long)h.last.lcdCommandWrites);
    CHECK(h.last.lcdPixelWrites > 1000);  // the BSP really painted the panel
    CHECK(h.mb[MB_LOOP] > 0);             // main loop reached

    // ---------------------------------------------------------------
    std::printf("\n--- Test 1: PA15 pulse input -> firmware capture ---\n");
    // default closed-loop phase (0) already has the TIM2 capture running
    const double freqs[] = {400.0, 1000.0, 5000.0, 20000.0};
    for (double f : freqs) {
        h.setPulse(f);
        CHECK(h.runVirtualMs(120.0));  // several capture periods
        h.refresh();
        char what[64];
        std::snprintf(what, sizeof(what),
                      "PA15 %.0f Hz -> firmware capture [Hz]", f);
        checkNear(what, double(h.mb[MB_PA15_HZ]), f, f <= 1000.0 ? 2.0 : 5.0);
        CHECK(h.mb[MB_PA15_HZ] > 0);
    }

    // ---------------------------------------------------------------
    std::printf("\n--- Test 1b: PB4 pulse input -> firmware capture ---\n");
    for (double f : freqs) {
        h.setPb4Pulse(f);
        CHECK(h.runVirtualMs(120.0));
        h.refresh();
        char what[64];
        std::snprintf(what, sizeof(what),
                      "PB4 %.0f Hz -> firmware capture [Hz]", f);
        checkNear(what, double(h.mb[MB_PB4_HZ]), f, f <= 1000.0 ? 2.0 : 5.0);
        CHECK(h.mb[MB_PB4_HZ] > 0);
    }
    // both inputs at the same time, independent measurements
    h.setPulse(2000.0);
    h.setPb4Pulse(7000.0);
    CHECK(h.runVirtualMs(150.0));
    h.refresh();
    checkNear("PA15 while PB4 also runs [Hz]", double(h.mb[MB_PA15_HZ]), 2000.0,
              3.0);
    checkNear("PB4 while PA15 also runs [Hz]", double(h.mb[MB_PB4_HZ]), 7000.0,
              3.0);
    h.setPb4Pulse(7000.0, /*on=*/false);

    // ---------------------------------------------------------------
    std::printf("\n--- Test 2: R37 / R38 ADC through the wired channels ---\n");
    struct AdcCase {
        const char* name;
        double volts;
        double code;  // expected 12-bit code
    };
    const AdcCase cases[] = {
        {"0 V", 0.0, 0.0}, {"1.65 V", 1.65, 2047.5}, {"3.3 V", 3.3, 4095.0}};
    for (const AdcCase& c : cases) {
        h.sim.setR37Voltage(c.volts);
        h.sim.setR38Voltage(c.volts);
        CHECK(h.runVirtualMs(60.0));
        h.refresh();
        char what[64];
        std::snprintf(what, sizeof(what), "R37 (ADC2_IN15) %s -> code",
                      c.name);
        checkNear(what, double(h.mb[MB_ADC_R37]), c.code,
                  c.code == 0.0 ? 100.0 : 1.0);
        std::snprintf(what, sizeof(what), "R38 (ADC1_IN11) %s -> code",
                      c.name);
        checkNear(what, double(h.mb[MB_ADC_R38]), c.code,
                  c.code == 0.0 ? 100.0 : 1.0);
        checkNear("R37 millivolts", double(h.mb[MB_MV_R37]), c.volts * 1000.0,
                  1.5);
    }

    // ---------------------------------------------------------------
    std::printf("\n--- Test 3: PA7 PWM measured at the pin ---\n");
    CHECK(h.setPhase(2));  // fixed 1000 Hz / 50 %
    CHECK(h.runVirtualMs(150.0));
    DigitalSignalMonitor::Stats w = h.pa7();
    checkNear("PA7 frequency [Hz]", w.frequencyHz, 1000.0, 2.0);
    checkNear("PA7 duty [%]", w.duty * 100.0, 50.0, 4.0);
    CHECK(w.active);
    CHECK(h.setPhase(3));  // fixed 2000 Hz / 25 %
    CHECK(h.runVirtualMs(150.0));
    w = h.pa7();
    checkNear("PA7 frequency [Hz]", w.frequencyHz, 2000.0, 2.0);
    checkNear("PA7 duty [%]", w.duty * 100.0, 25.0, 4.0);

    // ---------------------------------------------------------------
    std::printf("\n--- Test 4: wrong AF (PWM configured, PA7 not routed) ---\n");
    CHECK(h.setPhase(1));
    CHECK(h.runVirtualMs(100.0));
    h.refresh();
    const uint64_t edgesBefore = h.pa7().edges;
    const uint32_t fwHz = h.mb[MB_PA7_HZ];
    CHECK(h.runVirtualMs(100.0));
    h.refresh();
    const DigitalSignalMonitor::Stats wNoAf = h.pa7();
    std::printf("  firmware PWM config = %u Hz, pin edges %llu -> %llu\n", fwHz,
                (unsigned long long)edgesBefore,
                (unsigned long long)wNoAf.edges);
    CHECK(fwHz == 1000u);                  // the timer itself is configured
    CHECK(wNoAf.edges == edgesBefore);     // but not a single edge on the pin
    CHECK(!wNoAf.active);
    CHECK(wNoAf.frequencyHz == 0.0);

    // ---------------------------------------------------------------
    std::printf("\n--- Test 5: closed loop (16th provincial-style task) ---\n");
    CHECK(h.setPhase(0));  // back to the ADC-driven loop
    h.sim.setR37Voltage(1.20);  // -> duty
    h.sim.setR38Voltage(2.50);  // -> frequency
    h.setPulse(2800.0);
    CHECK(h.runVirtualMs(250.0));
    h.refresh();
    DigitalSignalMonitor::Stats wLoop = h.pa7();
    const uint32_t setHz = h.mb[MB_PA7_HZ];
    const uint32_t dutyPm = h.mb[MB_PA7_DUTY];
    std::printf("  R37=%u mV R38=%u mV -> firmware PA7 %u Hz / %u.%u %%%, "
                "pin %u Hz / %.1f %%, PA15 %u Hz, alarm=%u\n",
                h.mb[MB_MV_R37], h.mb[MB_MV_R38], setHz, dutyPm / 10u,
                dutyPm % 10u, unsigned(w.frequencyHz), w.duty * 100.0,
                h.mb[MB_PA15_HZ], h.mb[MB_ALARM]);
    // firmware's own conversion of the two potentiometers
    checkNear("firmware PWM frequency from R38 [Hz]", double(setHz), 4030.0,
              2.0);
    checkNear("firmware PWM duty from R37 [%]", double(dutyPm) / 10.0, 36.0,
              3.0);
    // ... and what actually appears on the pin
    checkNear("PA7 pin frequency [Hz]", wLoop.frequencyHz, double(setHz), 2.0);
    checkNear("PA7 pin duty [%]", wLoop.duty * 100.0, double(dutyPm) / 10.0,
              4.0);
    // firmware's own comparison: |2800 - 4030| = 1230 Hz > 1000 -> ALARM
    CHECK(h.mb[MB_ALARM] == 1u);
    CHECK(ledMask(h.last) == 0xFFu);  // firmware lights every LED on alarm

    // move the input close to the output: |3800 - 4030| = 230 Hz -> no alarm
    h.setPulse(3800.0);
    CHECK(h.runVirtualMs(250.0));
    h.refresh();
    std::printf("  PA15 -> %u Hz: diff vs %u Hz, alarm=%u, LEDs=0x%02X\n",
                h.mb[MB_PA15_HZ], h.mb[MB_PA7_HZ], h.mb[MB_ALARM],
                ledMask(h.last));
    CHECK(h.mb[MB_ALARM] == 0u);
    CHECK(ledMask(h.last) != 0xFFu);  // heartbeat only

    // and the pin still tracks the firmware configuration
    wLoop = h.pa7();
    checkNear("PA7 pin frequency [Hz]", wLoop.frequencyHz, double(h.mb[MB_PA7_HZ]),
              2.0);

    // pulse source off -> the firmware's measurement goes stale, no alarm
    h.setPulse(3800.0, /*on=*/false);
    CHECK(h.runVirtualMs(150.0));
    h.refresh();
    std::printf("  pulse source off: PA15 stays at %u Hz (last capture)\n",
                h.mb[MB_PA15_HZ]);

    h.refresh();
    std::printf("\nanalog_selftest summary: cycles=%llu (%.2f s virtual) "
                "wall=%lld ms\n",
                (unsigned long long)h.last.cycles,
                double(h.last.cycles) / 80.0e6, (long long)wall.elapsed());

    if (g_failures == 0) {
        std::printf("analog_selftest: PASS\n");
        return 0;
    }
    std::printf("analog_selftest: %d failure(s)\n", g_failures);
    return 1;
}