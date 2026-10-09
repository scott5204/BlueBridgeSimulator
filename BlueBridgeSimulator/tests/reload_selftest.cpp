// Regression test for "load another firmware after running one and it does not
// run" (found 2026-09-26 by the user: Open -> Run -> Reset -> Open -> Run):
//   * ROOT CAUSE: the 1:1 pacing compared the ABSOLUTE virtual cycle counter
//     against the wall clock; soc_->reset() keeps cycles_, so every run after
//     the first saw virtualMs already huge, runTick() broke out instantly and
//     Run did nothing (the old "Max speed" path used to hide this). Fixed by
//     measuring virtual time from the start of each run.
//   * loadFirmware() also refused to load while running (now it auto-pauses).
//
// Firmware signatures used (both live at 0x20000000):
//   firmware/test_usart : mailbox[7] = BRR (9600 8N1 -> 0x208D), transmits bytes
//   firmware/test_full  : mailbox[7] = LED speed (200 ms), never transmits
// So "which code is really executing" is unambiguous.
//
// Exit code 0 = pass.
#include <QCoreApplication>
#include <QElapsedTimer>

#include <cstdio>

#include "sim/Simulator.h"

namespace {
constexpr uint32_t kMailBase = 0x20000000u;
constexpr int MB_MAGIC = 0, MB_PAGE = 2, MB_LED = 3, MB_SPEED = 7;

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
}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    qRegisterMetaType<SimSnapshot>("SimSnapshot");
    Simulator sim;
    SimSnapshot snap;
    uint64_t cycles = 0;
    QObject::connect(&sim, &Simulator::stateChanged,
                     [&](const SimSnapshot& s) { snap = s; cycles = s.cycles; });
    const QString dir = QString::fromLocal8Bit(
        argc > 1 ? argv[1] : "firmware/");
    const auto run = [&](uint64_t ms) {
        const uint64_t target = cycles + ms * 80000ull;
        while (cycles < target) sim.slotStep(200);
    };

    // ---- 1) firmware A: runtest_usart, run it for a while ----------------
    sim.loadFirmware(dir + "test_usart/test_usart.hex");
    run(400);
    const uint32_t aBrr = mail(sim, 7);
    const uint64_t aTx = snap.usart1TxTotal;
    std::printf("A) usart: mailbox[7]=0x%04X (expect 0x208D = BRR), tx=%llu\n",
                aBrr, (unsigned long long)aTx);
    CHECK(aBrr == 0x208Du);
    CHECK(aTx > 0u);  // its TX firmware really ran

    // ---- 2) user presses Run, then picks another firmware ----------------
    sim.startRun();
    CHECK(sim.isRunning());
    sim.loadFirmware(dir + "test_full/test_full.hex");
    CHECK(!sim.isRunning());  // must have auto-paused and loaded
    if (sim.isRunning()) {    // slotStep would block forever otherwise
        std::printf("reload_selftest: FAILED (still running, load refused)\n");
        return 1;
    }
    const uint64_t txAfterLoad = snap.usart1TxTotal;
    run(500);

    // ---- 3) pressing Run must actually advance the virtual clock -----------
    // This is the regression: with the absolute-counter pacing, runTick()
    // returned immediately and the firmware stood still.
    const uint64_t cyclesBefore = snap.cycles;
    sim.startRun();
    CHECK(sim.isRunning());
    QElapsedTimer w;
    w.start();
    while (w.elapsed() < 400) {
        QMetaObject::invokeMethod(&sim, "runTick", Qt::DirectConnection);
    }
    sim.pause();
    const uint64_t advanced = snap.cycles - cyclesBefore;
    std::printf("C) run: virtual clock advanced %llu cycles (%.1f ms) in 400 ms "
                "of wall time\n",
                (unsigned long long)advanced, double(advanced) / 80000.0);
    CHECK(advanced > 8000000u);  // > 100 ms of virtual time: it really ran

    // ---- 4) firmware B must be the one executing now ---------------------
    run(500);
    const uint32_t bSpeed = mail(sim, 7);
    const uint32_t bMagic = mail(sim, MB_MAGIC);
    const uint32_t bLed = mail(sim, MB_LED);
    std::printf("B) full : mailbox[0]=0x%08X mailbox[7]=%u (expect 0xC0FFEE05 / 200), "
                "led=0x%02X, tx %llu -> %llu\n",
                bMagic, bSpeed, bLed, (unsigned long long)txAfterLoad,
                (unsigned long long)snap.usart1TxTotal);
    CHECK(bMagic == 0xC0FFEE05u);
    CHECK(bSpeed == 200u);                 // test_full's speed word, not the BRR
    CHECK(bLed != 0u && (bLed & (bLed - 1u)) == 0u);  // single-bit chaser
    CHECK(mail(sim, MB_PAGE) == 0u);
    CHECK(snap.usart1TxTotal == txAfterLoad);  // the old TX code is NOT running

    if (g_failures == 0) {
        std::printf("reload_selftest: PASS (second firmware really executes)\n");
        return 0;
    }
    std::printf("reload_selftest: %d FAILURE(S)\n", g_failures);
    return 1;
}