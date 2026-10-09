// Headless acceptance test for firmware/test_full -- the firmware the user
// drives by hand in the GUI (running light / LCD pages / keys / signal in+out).
// It protects the two things that were broken once:
//   * short key presses being dropped (5 ms scan + 20 ms debounce ate half of
//     the clicks at <1x real time) -> Test 2 uses a 12 ms press
//   * drawing in the wrong coordinate frame on the LCD (the BSP's geometry
//     primitives work in the controller's native frame) -> Test 5 checks that
//     every page actually has content
//
// Test 1  LED chaser: several distinct single-bit patterns, direction + speed
// Test 2  keys: a normal press and a 12 ms short press are both counted
// Test 3  key actions: B1 direction, B2 speed, B3 PWM preset
// Test 4  PWM output: PA7 really toggles at the new preset (board monitor)
// Test 5  capture inputs: PA15 @2 kHz, PB4 @3 kHz measured by the firmware
// Test 6  LCD pages: switch through 0/1/2 and check the content of each
// Test 7  final key counters
//
// Exit code 0 = pass.
#include <QCoreApplication>

#include <cmath>
#include <cstdio>
#include <set>
#include <vector>

#include "sim/Simulator.h"

namespace {
constexpr uint32_t kMailBase = 0x20000000u;
constexpr uint32_t kMagic = 0xC0FFEE05u;

enum {
    MB_MAGIC = 0, MB_CMD, MB_PAGE, MB_LED, MB_KEYCNT, MB_KEYNOW, MB_DIR,
    MB_SPEED, MB_PA15_HZ, MB_PB4_HZ, MB_PWM_PRESET, MB_PA7_HZ, MB_PA7_DUTY,
    MB_MV_R37, MB_MV_R38, MB_LOOP, MB_REDRAWS,
};

int g_failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                               \
        }                                                               \
    } while (0)

uint32_t mail(Simulator& sim, int i) {
    uint32_t v = 0;
    sim.debugSramWord(kMailBase + 4u * uint32_t(i), v);
    return v;
}

uint32_t keyCount(Simulator& sim, int index) {
    return (mail(sim, MB_KEYCNT) >> (8 * index)) & 0xFFu;
}

class Runner {
public:
    explicit Runner(Simulator& sim) : sim_(sim) {
        QObject::connect(&sim, &Simulator::stateChanged,
                         [this](const SimSnapshot& s) { snap_ = s; });
    }
    void run(uint64_t ms) {
        const uint64_t target = snap_.cycles + ms * 80000ull;
        while (snap_.cycles < target) sim_.slotStep(200);
    }
    void press(int index, uint64_t heldMs) {
        sim_.setButton(index, true);
        run(heldMs);
        sim_.setButton(index, false);
        // The tick only counts the press; the main loop consumes it, and it may
        // be busy with an LCD redraw (tens of ms of virtual time) -- give it
        // enough time to settle before the caller asserts anything.
        run(150);
    }
    const SimSnapshot& snap() const { return snap_; }

private:
    Simulator& sim_;
    SimSnapshot snap_{};
};

int lcdColours(Simulator& sim) {
    std::vector<uint16_t> fb;
    sim.copyLcdFramebuffer(fb);
    std::set<uint16_t> s(fb.begin(), fb.end());
    return int(s.size());
}
}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    qRegisterMetaType<SimSnapshot>("SimSnapshot");
    Simulator sim;
    Runner run(sim);
    sim.loadFirmware(QString::fromLocal8Bit(
        argc > 1 ? argv[1] : "firmware/test_full/test_full.hex"));
    run.run(400);  // boot + LCD init

    // ---- Test 1: LED chaser ------------------------------------------------
    std::set<uint32_t> ledStates;
    for (int i = 0; i < 10; i++) {
        ledStates.insert(mail(sim, MB_LED));
        run.run(120);
    }
    CHECK(mail(sim, MB_MAGIC) == kMagic);
    CHECK(ledStates.size() >= 3);  // the chaser moved
    for (uint32_t v : ledStates) CHECK(v != 0u && (v & (v - 1u)) == 0u);  // one bit
    CHECK(mail(sim, MB_PAGE) == 0u);
    CHECK(lcdColours(sim) >= 2);  // page 0 has text

    // ---- Test 2: normal press + SHORT press are both counted --------------
    const uint32_t b1Before = keyCount(sim, 0);
    run.press(0, 60);                       // normal
    CHECK(keyCount(sim, 0) == b1Before + 1u);
    run.press(0, 12);                       // short click (the regression)
    CHECK(keyCount(sim, 0) == b1Before + 2u);
    run.press(0, 12);
    CHECK(keyCount(sim, 0) == b1Before + 3u);
    CHECK(mail(sim, MB_DIR) == 0u);         // 3 presses -> odd -> direction <
    CHECK(mail(sim, MB_KEYNOW) == 0u);      // released

    // ---- Test 3: key actions ----------------------------------------------
    run.press(1, 40);                       // B2: speed
    CHECK(mail(sim, MB_SPEED) == 100u);     // 200 -> 100 ms
    run.press(2, 40);                       // B3: PWM preset
    CHECK(mail(sim, MB_PWM_PRESET) == 1u);
    CHECK(mail(sim, MB_PA7_HZ) == 2000u);
    CHECK(mail(sim, MB_PA7_DUTY) == 250u);

    // ---- Test 4: PA7 really toggles at the new preset ---------------------
    run.run(200);
    CHECK(std::fabs(run.snap().pa7Frequency - 2000.0) < 20.0);
    CHECK(std::fabs(run.snap().pa7Duty - 0.25) < 0.02);  // pa7Duty is 0..1

    // ---- Test 5: capture inputs -------------------------------------------
    sim.setPulseEnabled(true);
    sim.setPulseFrequency(2000.0);
    sim.setPb4PulseEnabled(true);
    sim.setPb4PulseFrequency(3000.0);
    run.run(600);
    const uint32_t pa15 = mail(sim, MB_PA15_HZ);
    const uint32_t pb4 = mail(sim, MB_PB4_HZ);
    std::printf("inputs: PA15=%u Hz (expect ~2000), PB4=%u Hz (expect ~3000)\n",
                pa15, pb4);
    CHECK(pa15 > 1980u && pa15 < 2020u);
    CHECK(pb4 > 2940u && pb4 < 3060u);  // 1 us capture quantisation

    // ---- Test 6: LCD pages -------------------------------------------------
    run.press(3, 40);
    CHECK(mail(sim, MB_PAGE) == 1u);
    CHECK(lcdColours(sim) >= 6);  // 8 colour bars + text
    run.press(3, 40);
    CHECK(mail(sim, MB_PAGE) == 2u);
    CHECK(lcdColours(sim) >= 2);
    run.press(3, 40);
    CHECK(mail(sim, MB_PAGE) == 0u);

    // ---- Test 7: counters --------------------------------------------------
    CHECK(keyCount(sim, 0) == b1Before + 3u);
    CHECK(keyCount(sim, 1) == 1u);
    CHECK(keyCount(sim, 2) == 1u);
    CHECK(keyCount(sim, 3) == 3u);
    CHECK(mail(sim, MB_REDRAWS) >= 3u);

    

    

    if (g_failures == 0) {
        std::printf("full_selftest: PASS (ledStates=%zu, pages=3, keys ok)\n",
                    ledStates.size());
        return 0;
    }
    std::printf("full_selftest: %d FAILURE(S)\n", g_failures);
    return 1;
}