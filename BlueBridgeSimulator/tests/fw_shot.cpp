// Firmware bring-up helper: run any firmware image headlessly in the
// simulator for a given amount of VIRTUAL time, then report the board state
// and dump the LCD panel as a PNG.
//
// usage: fw_shot <firmware.hex|bin> [virtual-ms] [out.png] [sram-addr]
//   virtual-ms  default 1500 (virtual milliseconds, not wall clock)
//   out.png     default fw_panel.png
//   sram-addr   optional: also dump 16 words at this address (bring-up aid)
//
// Exit code 0 on success, 1 on fault/load error.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>

#include "board/lcd/LcdController.h"
#include "sim/Simulator.h"

static std::vector<uint16_t> g_panel;

static uint32_t ledMask(const SimSnapshot& s) {
    uint32_t m = 0;
    for (int i = 0; i < 8; i++)
        if (s.leds[i]) m |= 1u << i;
    return m;
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    qRegisterMetaType<SimSnapshot>("SimSnapshot");

    if (argc < 2) {
        std::printf("usage: fw_shot <firmware.hex|bin> [virtual-ms] [out.png]\n");
        return 2;
    }
    const double budgetMs = (argc > 2) ? std::atof(argv[2]) : 1500.0;
    const QString outPath =
        (argc > 3) ? QString::fromLocal8Bit(argv[3]) : QString("fw_panel.png");

    Simulator sim;
    SimSnapshot last;
    bool faulted = false, loadFailed = false;
    std::set<uint32_t> ledStates;

    QObject::connect(&sim, &Simulator::stateChanged, [&](const SimSnapshot& s) {
        last = s;
        ledStates.insert(ledMask(s));
    });
    QObject::connect(&sim, &Simulator::faulted, [&](const QString& e) {
        faulted = true;
        std::printf("FAULT: %s\n", e.toUtf8().constData());
    });
    QObject::connect(&sim, &Simulator::firmwareLoadFailed,
                     [&](const QString& e) {
                         loadFailed = true;
                         std::printf("load failed: %s\n", e.toUtf8().constData());
                     });

    sim.loadFirmware(QString::fromLocal8Bit(argv[1]));
    if (loadFailed || !sim.firmwareLoaded()) return 1;

    QElapsedTimer wall;
    wall.start();
    while (wall.elapsed() < 60000) {
        sim.slotStep(20);
        if (faulted) break;
        const double virtualMs =
            last.sysclkHz ? double(last.cycles) * 1000.0 / double(last.sysclkHz)
                          : 0.0;
        if (virtualMs >= budgetMs) break;
    }

    std::printf(
        "fw_shot: cycles=%llu sysclk=%u MHz virtual=%.1f ms wall=%lld ms\n",
        (unsigned long long)last.cycles, last.sysclkHz / 1000000u,
        last.sysclkHz ? double(last.cycles) * 1000.0 / double(last.sysclkHz) : 0.0,
        (long long)wall.elapsed());
    std::printf("fw_shot: leds=0x%02X distinctLedStates=%zu lcdCmds=%llu "
                "lcdPixels=%llu\n",
                ledMask(last), ledStates.size(),
                (unsigned long long)last.lcdCommandWrites,
                (unsigned long long)last.lcdPixelWrites);
    std::printf("fw_shot: pc=0x%08X lr=0x%08X sp=0x%08X ipsr=%u ticks=%u\n",
                last.pc, last.lr, last.sp, last.ipsr, last.systickCvr);
    std::printf("fw_shot: systickCsr=0x%08X rvr=%u primask=%u\n",
                last.systickCsr, last.systickRvr, last.primask);
    if (argc > 4) {  // optional: peek 16 words at a debug address
        const uint32_t base = std::strtoul(argv[4], nullptr, 0);
        std::printf("fw_shot: sram[0x%08X] =", base);
        for (int i = 0; i < 16; i++) {
            uint32_t v = 0;
            sim.debugSramWord(base + 4u * uint32_t(i), v);
            std::printf(" %08X", v);
        }
        std::printf("\n");
    }

    // Board pin waveform monitor (PA7 = TIM3_CH2 / TIM17_CH1 PWM output).
    const DigitalSignalMonitor::Stats w = sim.pa7Waveform();
    std::printf("fw_shot: PA7 level=%s %s freq=%.1f Hz duty=%.1f %% edges=%llu\n",
                w.level ? "high" : "low", w.active ? "toggling" : "idle      ",
                w.frequencyHz, w.duty * 100.0, (unsigned long long)w.edges);

    // Optional firmware mailbox: the CT117E test firmwares keep a table of
    // words at 0x20000000 (.selfdata). Printed only when it is non-empty, so
    // arbitrary firmware stays readable.
    uint32_t mb[8] = {};
    bool hasMb = false;
    for (int i = 0; i < 8; i++) {
        sim.debugSramWord(0x20000000u + 4u * uint32_t(i), mb[i]);
        if (mb[i] != 0) hasMb = true;
    }
    if (hasMb) {
        std::printf("fw_shot: mailbox[0x20000000] =");
        for (int i = 0; i < 8; i++) std::printf(" %u", mb[i]);
        std::printf("\n");
    }

    sim.copyLcdFramebuffer(g_panel);
    std::set<uint16_t> colors;
    size_t nonBlack = 0;
    for (uint16_t p : g_panel) {
        colors.insert(p);
        if (p != 0x0000) ++nonBlack;
    }
    std::printf("fw_shot: panel %zux%zu, distinct colours=%zu, non-black=%zu\n",
                size_t(LcdController::kPanelWidth),
                size_t(LcdController::kPanelHeight), colors.size(), nonBlack);

    QImage img(LcdController::kPanelWidth, LcdController::kPanelHeight,
               QImage::Format_RGB888);
    for (int py = 0; py < LcdController::kPanelHeight; py++) {
        uchar* out = img.scanLine(py);
        for (int px = 0; px < LcdController::kPanelWidth; px++) {
            const uint16_t p = g_panel[size_t(py) * LcdController::kPanelWidth + px];
            const uint32_t r5 = (p >> 11) & 0x1Fu, g6 = (p >> 5) & 0x3Fu,
                           b5 = p & 0x1Fu;
            *out++ = uchar((r5 << 3) | (r5 >> 2));
            *out++ = uchar((g6 << 2) | (g6 >> 4));
            *out++ = uchar((b5 << 3) | (b5 >> 2));
        }
    }
    if (img.save(outPath)) {
        std::printf("fw_shot: panel written to %s\n",
                    outPath.toLocal8Bit().constData());
    } else {
        std::printf("fw_shot: could not write %s\n",
                    outPath.toLocal8Bit().constData());
    }
    return faulted ? 1 : 0;
}