// Stage-5 headless acceptance test: USART1 + the virtual serial terminal.
//
// Phase A checks the Usart model directly (register semantics, BRR/PRESC/OVER8
// baud decoding, byte-level timing, ORE + ICR, the clock gate).
//
// Phase B runs the real test firmware on the emulated Cortex-M4 and uses the
// virtual PC terminal exactly like the GUI does:
//   * the host only TYPES bytes into the terminal (board serial peer) and reads
//     what the firmware transmitted -- it never touches a USART register
//   * the firmware configures USART1 itself (RCC gate, PA9/PA10 = AF7, BRR,
//     UE/TE/RE, RXNEIE + NVIC) and owns the line protocol
//
// Test 1  TX: "HELLO" reaches the terminal
// Test 2  RX + echo: host "ABC" -> firmware echo
// Test 3  RXNE interrupt: the real USART1_IRQHandler ran (ISR counters)
// Test 4  TX timing @9600: 100 bytes spread over ~104 ms of virtual time
// Test 4b baud from BRR: the same burst at 115200 takes ~9 ms
// Test 5  wrong AF: TX drops every byte, RX leaves RXNE at 0
// Test 6  overrun: ORE set when RDR is not read, cleared through ICR
// Test 7  15th competition protocol: (x,y) -> "Got it", ? -> "Idle",
//         # -> "(x,y)"
//
// Exit code 0 = pass.
#include <QCoreApplication>
#include <QElapsedTimer>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "sim/Simulator.h"
#include "stm32/usart/Usart.h"

// firmware mailbox, placed at 0x20000000 by the linker script
static constexpr uint32_t kMailBase = 0x20000000u;
static constexpr int kMailWords = 20;
static constexpr uint32_t kMailMagic = 0xC0FFEE05u;

enum {
    MB_MAGIC = 0,
    MB_CMD = 1,
    MB_PHASE = 2,
    MB_TX_BYTES = 3,
    MB_RX_BYTES = 4,
    MB_RX_IRQ = 5,
    MB_LAST_BYTE = 6,
    MB_BRR = 7,
    MB_BURST_9600 = 8,
    MB_ORE_SEEN = 9,
    MB_ORE_CLEARED = 10,
    MB_ORE_RDR = 11,
    MB_REPLIES = 12,
    MB_X = 13,
    MB_Y = 14,
    MB_LOOP = 15,
    MB_TXE_TIMEOUTS = 16,
    MB_BURST_115200 = 17,
    MB_RX_AT_PHASE = 18,
};

static int g_failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                               \
        }                                                               \
    } while (0)

// ---------------------------------------------------------------------------
// Phase A: the Usart model on its own (no CPU, no firmware)
// ---------------------------------------------------------------------------
static void checkNear(const char* what, double value, double expected,
                      double tolPercent) {
    const double err = std::fabs(value - expected) * 100.0 /
                       (expected != 0.0 ? std::fabs(expected) : 1.0);
    const bool ok = err <= tolPercent;
    std::printf("  %-46s = %12.2f (expected %10.2f, err %6.2f %%) %s\n", what,
                value, expected, err, ok ? "ok" : "FAIL");
    if (!ok) g_failures++;
}

static void phaseA() {
    std::printf("--- Phase A: USART1 model (register + timing level) ---\n");
    Usart u(0x40013800u, {"USART1", 37});
    u.setClockProviders([] { return 80'000'000u; },
                        [] { return 80'000'000u; });
    int irqCount = 0;
    std::vector<uint8_t> tx;
    u.setIrqCallback([&](int irq) { if (irq == 37) irqCount++; });
    u.setTxSink([&](uint8_t b) { tx.push_back(b); });
    u.reset();

    auto wr = [&](uint32_t off, uint32_t v) { u.busWrite(u.base() + off, v, 4); };
    auto rd = [&](uint32_t off) { return u.busRead(u.base() + off, 4); };

    // ---- A1: clock gate (APB2ENR.USART1EN) ----
    CHECK(!u.clockEnabled());
    wr(Usart::R_CR1, 0xFFFFFFFFu);
    CHECK(rd(Usart::R_CR1) == 0u);  // unclocked: reads 0, writes dropped
    u.setClockEnabled(true);
    CHECK(u.clockEnabled());

    // ---- A2: baud decoding (BRR + PRESC + OVER8) ----
    wr(Usart::R_BRR, 0x208Du);  // 8333 -> 9600.38 baud at 80 MHz PCLK2
    checkNear("BRR 0x208D -> baud", double(u.baudRate()), 9600.0, 0.01);
    CHECK(u.bitCycles() == 8333u);
    CHECK(u.byteCycles() == 83330u);  // 8N1 = 10 bits -> 1.0416 ms
    checkNear("8N1 byte time [ms]", double(u.byteCycles()) * 1000.0 / 80.0e6,
              1.0416, 0.1);
    wr(Usart::R_PRESC, 1u);  // divide by 2 -> 4800
    checkNear("PRESC=1 -> baud", double(u.baudRate()), 4800.0, 0.01);
    wr(Usart::R_PRESC, 0u);
    // OVER8=1: factor 8 and a 3-bit fraction: mant 86, frac 6 -> 694 cycles
    wr(Usart::R_CR1, Usart::kCr1Over8);
    wr(Usart::R_BRR, (86u << 4) | 6u);
    checkNear("OVER8=1 BRR 0x566 -> baud", double(u.baudRate()), 115274.0,
              0.05);
    wr(Usart::R_CR1, 0u);
    wr(Usart::R_BRR, 0x208Du);
    // CR2 stop bits change the frame length (2 stop bits -> 11 bits)
    wr(Usart::R_CR2, 2u << 12);
    CHECK(u.stopBitsHalf() == 4u);
    CHECK(u.byteCycles() == 8333u * 22u / 2u);
    wr(Usart::R_CR2, 0u);

    // ---- A3: transmitter handshake (TDR -> shift -> line) ----
    u.setNowCycles(0);
    wr(Usart::R_CR1, Usart::kCr1Ue | Usart::kCr1Te);  // UE + TE
    CHECK(u.txe());                                   // TDR empty, ready
    wr(Usart::R_TDR, 'A');
    CHECK(tx.empty());  // nothing on the line yet
    u.advance(83330u);
    CHECK(tx.size() == 1 && tx[0] == 'A');
    CHECK(u.tc());  // last stop bit completed
    CHECK(u.txe());
    wr(Usart::R_ICR, Usart::kIcrTccf);
    CHECK(!u.tc());

    // two bytes back to back: the second waits in TDR (TXE low)
    tx.clear();
    u.setNowCycles(100000u);
    wr(Usart::R_TDR, 'X');
    wr(Usart::R_TDR, 'Y');
    CHECK(!u.txe());
    u.advance(100000u + 83330u);
    CHECK(tx.size() == 1 && tx[0] == 'X');
    CHECK(u.txe());  // TDR moved into the shift register
    CHECK(!u.tc());
    u.advance(100000u + 2u * 83330u);
    CHECK(tx.size() == 2 && tx[1] == 'Y');
    CHECK(u.tc());

    // TE=0: writes are not transmitted
    tx.clear();
    wr(Usart::R_CR1, Usart::kCr1Ue);  // TE = 0
    u.setNowCycles(300000u);
    wr(Usart::R_TDR, 'Z');
    u.advance(300000u + 3u * 83330u);
    CHECK(tx.empty());

    // ---- A4: receiver + RXNE interrupt ----
    wr(Usart::R_CR1, Usart::kCr1Ue | Usart::kCr1Re | Usart::kCr1Rxneie);
    irqCount = 0;
    u.receive('x');
    CHECK(u.rxne());
    CHECK(irqCount == 1);  // RXNEIE pends the IRQ
    CHECK(rd(Usart::R_RDR) == 'x');
    CHECK(!u.rxne());  // reading RDR clears RXNE

    // ---- A5: overrun (ORE) and its two documented clear paths ----
    u.receive('A');
    u.receive('B');  // RDR not read -> overrun, 'A' kept, 'B' lost
    CHECK(u.ore());
    CHECK(rd(Usart::R_RDR) == 'A');
    CHECK(u.ore());  // ORE is NOT cleared by a plain RDR read
    wr(Usart::R_ICR, Usart::kIcrOrecf);
    CHECK(!u.ore());
    // read ISR then RDR also clears ORE (RM0440 sequence)
    u.receive('C');
    u.receive('D');
    CHECK(u.ore());
    (void)rd(Usart::R_ISR);  // arms the sequence
    CHECK(rd(Usart::R_RDR) == 'C');
    CHECK(!u.ore());

    // ---- A6: RXFRQ flushes RDR ----
    u.receive('E');
    CHECK(u.rxne());
    wr(Usart::R_RQR, Usart::kRqrRxfrq);
    CHECK(!u.rxne());

    // ---- A7: UC=0 / RE=0 drop incoming bytes ----
    wr(Usart::R_CR1, Usart::kCr1Ue);  // RE = 0
    u.receive('F');
    CHECK(!u.rxne());
    // ---- A8: clock gate closed again: traffic stops ----
    u.setClockEnabled(false);
    u.receive('G');
    CHECK(!u.rxne());
    CHECK(rd(Usart::R_ISR) == 0u);
    CHECK(u.baudRate() == 0u);  // no kernel clock through the gate

    // ---- A9: 100-byte burst timing, driven the way the firmware drives it ----
    // (poll TXE, write TDR, let the model schedule exactly one frame at a time)
    struct BurstCase {
        const char* name;
        uint32_t brr;
        uint32_t over8;
        uint32_t expectCycles;
    };
    const BurstCase cases[] = {
        {"9600 (BRR 0x208D)", 0x208Du, 0u, 99u * 83330u},
        {"115200 (BRR 0x2B7)", 0x02B7u, 0u, 99u * 6950u},
        {"115200 OVER8 (BRR 0x566)", (86u << 4) | 6u, Usart::kCr1Over8,
         99u * 6940u},
    };
    for (const BurstCase& c : cases) {
        u.reset();
        u.setClockEnabled(true);
        u.setNowCycles(0);
        tx.clear();
        wr(Usart::R_BRR, c.brr);
        wr(Usart::R_CR1, Usart::kCr1Ue | Usart::kCr1Te | c.over8);
        uint64_t now = 0;
        int sent = 0;
        while (sent < 100 && now < 20'000'000u) {
            if (u.txe()) {
                wr(Usart::R_TDR, 'U');
                sent++;
            }
            uint64_t step = u.cyclesToNextEvent();
            if (step == ~0ull || step == 0) step = 1000;
            if (step > 40000u) step = 40000u;  // the simulator's batch cap
            now += step;
            u.setNowCycles(now);
            u.advance(now);
        }
        char what[64];
        std::snprintf(what, sizeof(what), "100-byte burst %s [cycles]",
                      c.name);
        checkNear(what, double(now), double(c.expectCycles), 2.0);
        // the last byte is still in flight (the firmware's measurement window
        // closes right after the last TDR write)
        CHECK(tx.size() >= 98u && tx.size() <= 100u);
    }
}

// ---------------------------------------------------------------------------
// Phase B: the test firmware on the emulated CPU + the virtual terminal
// ---------------------------------------------------------------------------
struct Host {
    Simulator sim;
    SimSnapshot last;
    bool faulted = false;
    bool loadFailed = false;
    uint32_t mb[kMailWords] = {};
    std::string mcuText;  // everything the firmware transmitted
    size_t mcuBytes = 0;

    Host() {
        QObject::connect(&sim, &Simulator::stateChanged,
                         [this](const SimSnapshot& s) {
                             last = s;
                             if (s.usart1TxPending > 0) {
                                 const std::vector<uint8_t> v =
                                     sim.takeUsart1TxBytes();
                                 mcuBytes += v.size();
                                 mcuText.append((const char*)v.data(), v.size());
                             }
                         });
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

    bool runToCycles(uint64_t target, qint64 wallMs = 30000, int batchStep = 20) {
        QElapsedTimer w;
        w.start();
        while (!faulted && last.cycles < target && w.elapsed() < wallMs)
            sim.slotStep(batchStep);
        if (last.cycles < target)
            std::printf("  [warn] runToCycles: target %llu not reached "
                        "(cycles=%llu)\n",
                        (unsigned long long)target,
                        (unsigned long long)last.cycles);
        return !faulted;
    }

    bool runVirtualMs(double ms, qint64 wallMs = 30000, int batchStep = 20) {
        const double hz = last.sysclkHz ? double(last.sysclkHz) : 1.0;
        return runToCycles(last.cycles + uint64_t(ms * hz / 1000.0), wallMs,
                           batchStep);
    }

    bool setPhase(uint32_t phase) {
        sim.debugSramWrite(kMailBase + 4u * MB_CMD, phase);
        QElapsedTimer w;
        w.start();
        for (int i = 0; i < 4000 && w.elapsed() < 10000; i++) {
            sim.slotStep(1);  // one batch: the handshake stays tight
            if (faulted) return false;
            refresh();
            if (mb[MB_PHASE] == phase) return true;
        }
        std::printf("phase %u never applied (mailbox phase=%u)\n", phase,
                    mb[MB_PHASE]);
        return false;
    }

    // the terminal types a line (exactly what the GUI Send button does)
    void sendLine(const std::string& text, bool crlf = true) {
        std::vector<uint8_t> bytes(text.begin(), text.end());
        if (crlf) {
            bytes.push_back('\r');
            bytes.push_back('\n');
        }
        sim.sendUsart1Bytes(bytes);
    }

    // wait until @needle shows up in the MCU -> PC stream
    bool waitForText(const std::string& needle, double stepMs = 20.0,
                     qint64 wallMs = 20000) {
        QElapsedTimer w;
        w.start();
        while (w.elapsed() < wallMs) {
            if (mcuText.find(needle) != std::string::npos) return true;
            if (faulted) return false;
            if (!runVirtualMs(stepMs, wallMs)) return false;
        }
        return mcuText.find(needle) != std::string::npos;
    }

    bool waitForOre(uint32_t* value, qint64 wallMs = 20000) {
        QElapsedTimer w;
        w.start();
        while (w.elapsed() < wallMs) {
            sim.slotStep(20);
            if (faulted) return false;
            refresh();
            *value = mb[MB_ORE_RDR];
            if (*value != 0u) return true;
        }
        return false;
    }
};

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    qRegisterMetaType<SimSnapshot>("SimSnapshot");

    phaseA();

    if (argc < 2) {
        std::printf("usage: usart_selftest <test_usart.hex>\n");
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
    h.runToCycles(8'000'000);
    // ---------------------------------------------------------------
    std::printf("\n--- Test 1: TX, the firmware's first transmission ---\n");
    CHECK(h.waitForText("HELLO"));
    h.refresh();
    CHECK(!h.faulted);
    CHECK(h.last.sysclkHz == 80'000'000u);
    CHECK(h.mb[MB_MAGIC] == kMailMagic);
    CHECK(h.mb[MB_BRR] == 0x208Du);         // 9600 configured through BRR
    CHECK(h.last.usart1ClockEnabled);
    CHECK(h.last.usart1Enabled && h.last.usart1TxEnabled &&
          h.last.usart1RxEnabled);
    CHECK(h.last.usart1Baud == 9600u);      // decoded, not hard-coded
    CHECK(h.last.usart1WordBits == 8u && h.last.usart1Parity == 0 &&
          h.last.usart1StopBitsHalf == 2u);
    std::printf("  terminal received %zu byte(s): \"%s\"\n", h.mcuBytes,
                h.mcuText.c_str());
    CHECK(h.mcuBytes >= 7u);  // "HELLO" + CRLF, byte by byte on the wire
    CHECK(h.mb[MB_TX_BYTES] >= 7u);
    CHECK(h.mb[MB_LOOP] > 0u);

    // ---------------------------------------------------------------
    std::printf("\n--- Test 2: RX + echo (host \"ABC\" -> firmware echo) ---\n");
    const size_t bytesBefore = h.mcuBytes;
    const uint32_t rxBefore = h.mb[MB_RX_BYTES];
    const uint32_t irqBefore = h.mb[MB_RX_IRQ];
    h.sendLine("ABC");
    CHECK(h.waitForText("ABC"));
    h.refresh();
    std::printf("  rx bytes %u -> %u, RXNE interrupts %u -> %u, last byte "
                "0x%02X\n",
                rxBefore, h.mb[MB_RX_BYTES], irqBefore, h.mb[MB_RX_IRQ],
                h.mb[MB_LAST_BYTE]);
    CHECK(h.mb[MB_RX_BYTES] == rxBefore + 5u);  // A B C CR LF
    CHECK(h.mb[MB_RX_IRQ] >= irqBefore + 5u);   // one RXNE interrupt each
    CHECK(h.mb[MB_LAST_BYTE] == '\n');
    CHECK(h.mcuBytes > bytesBefore);            // the echo really went out

    // ---------------------------------------------------------------
    std::printf("\n--- Test 3: the RXNE interrupt is a real exception ---\n");
    // The ISR counter only advances if the NVIC delivered USART1_IRQn = 37 and
    // the CPU executed the vector: IRQ entries == RX bytes (one per byte).
    std::printf("  USART1_IRQHandler entries: %u for %u received byte(s), "
                "last byte 0x%02X\n",
                h.mb[MB_RX_IRQ], h.mb[MB_RX_BYTES], h.mb[MB_LAST_BYTE]);
    CHECK(h.mb[MB_RX_IRQ] > 0u);
    CHECK(h.mb[MB_RX_IRQ] == h.mb[MB_RX_BYTES]);
    CHECK(h.last.exceptionReturns > 0u);  // exceptions really returned

    // ---------------------------------------------------------------
    std::printf("\n--- Test 4: TX timing @9600 (100 bytes ~ 104 ms) ---\n");
    const size_t burstBefore = h.mcuBytes;   // from the phase command on
    const uint64_t burstT0 = h.last.cycles;
    CHECK(h.setPhase(1));
    CHECK(h.runVirtualMs(40.0, 30000, 2));
    const size_t mid = h.mcuBytes - burstBefore;
    int guard = 0;
    while (h.mcuBytes - burstBefore < 100u && guard++ < 200)
        CHECK(h.runVirtualMs(2.0, 30000, 2));
    h.refresh();
    const double burstMs =
        double(h.last.cycles - burstT0) * 1000.0 / 80.0e6;
    std::printf("  after 40 ms virtual: %zu of 100 bytes; all 100 after %.2f "
                "ms of virtual time; firmware measured %u ms\n",
                mid, burstMs, h.mb[MB_BURST_9600]);
    CHECK(mid > 5 && mid < 60);   // far from instantaneous, far from done
    CHECK(h.mcuBytes - burstBefore == 100u);
    CHECK(mid < 100u);            // still not complete after 40 ms
    checkNear("100 bytes @9600 on the wire [ms]", burstMs, 104.2, 8.0);
    CHECK(h.mb[MB_BURST_9600] >= 95u && h.mb[MB_BURST_9600] <= 115u);
    CHECK(h.mb[MB_TXE_TIMEOUTS] == 0u);
    CHECK(h.last.usart1Tc);  // the last byte completed

    // ---------------------------------------------------------------
    std::printf("\n--- Test 4b: the same burst at 115200 (BRR decides) ---\n");
    const size_t fastBefore = h.mcuBytes;
    const uint64_t fastT0 = h.last.cycles;
    CHECK(h.setPhase(7));
    int guard2 = 0;
    while (h.mcuBytes - fastBefore < 100u && guard2++ < 200)
        CHECK(h.runVirtualMs(1.0, 30000, 2));
    h.refresh();
    const double fastMs = double(h.last.cycles - fastT0) * 1000.0 / 80.0e6;
    std::printf("  all 100 bytes after %.2f ms of virtual time, firmware "
                "measured %u ms, model baud %u\n",
                fastMs, h.mb[MB_BURST_115200], h.last.usart1Baud);
    CHECK(h.mcuBytes - fastBefore == 100u);
    checkNear("100 bytes @115200 on the wire [ms]", fastMs, 8.7, 30.0);
    CHECK(h.mb[MB_BURST_115200] <= 15u);
    CHECK(h.mb[MB_BRR] == 0x02B7u);
    CHECK(h.last.usart1Baud > 114000u && h.last.usart1Baud < 116000u);

    // ---------------------------------------------------------------
    std::printf("\n--- Test 5a: wrong TX AF -> the terminal receives 0 bytes "
                "---\n");
    const size_t noAfBefore = h.mcuBytes;
    const uint32_t txBefore = h.mb[MB_TX_BYTES];  // before the phase command
    CHECK(h.setPhase(2));  // firmware drops PA9 AF and sends "HELLO"
    CHECK(h.runVirtualMs(60.0));
    h.refresh();
    std::printf("  firmware transmitted %u -> %u byte(s), terminal got %zu "
                "(expected 0)\n",
                txBefore, h.mb[MB_TX_BYTES], h.mcuBytes - noAfBefore);
    CHECK(h.mb[MB_TX_BYTES] >= txBefore + 7u);  // the firmware really wrote TDR
    CHECK(h.mcuBytes == noAfBefore);            // but nothing left the pin

    // ---------------------------------------------------------------
    std::printf("\n--- Test 5b: wrong RX AF -> RXNE stays 0 ---\n");
    CHECK(h.setPhase(3));  // firmware drops PA10 AF
    const uint32_t rxBase = h.mb[MB_RX_BYTES];
    h.sendLine("XYZ");  // the peer drives the line, the USART must not see it
    CHECK(h.runVirtualMs(60.0));
    h.refresh();
    std::printf("  host sent 5 byte(s), firmware RX count %u -> %u, RXNE=%d\n",
                rxBase, h.mb[MB_RX_BYTES], h.last.usart1Rxne ? 1 : 0);
    CHECK(h.mb[MB_RX_BYTES] == rxBase);
    CHECK(h.mb[MB_RX_AT_PHASE] == rxBase);
    CHECK(!h.last.usart1Rxne);

    // ---------------------------------------------------------------
    std::printf("\n--- Test 6: overrun (ORE + ICR clear) ---\n");
    CHECK(h.setPhase(4));  // the firmware now waits for 'A' (unread) + 'B'
    {
        const std::vector<uint8_t> bytes = {'A', 'B'};  // back to back
        h.sim.sendUsart1Bytes(bytes);
    }
    uint32_t rdr = 0;
    CHECK(h.waitForOre(&rdr));
    h.refresh();
    std::printf("  ORE after the second byte: %u, after ICR.ORECF: %u, RDR "
                "read back 0x%02X ('%c')\n",
                h.mb[MB_ORE_SEEN], h.mb[MB_ORE_CLEARED], h.mb[MB_ORE_RDR],
                (char)h.mb[MB_ORE_RDR]);
    CHECK(h.mb[MB_ORE_SEEN] == 1u);      // ORE really got set
    CHECK(h.mb[MB_ORE_CLEARED] == 0u);   // ... and ICR cleared it
    CHECK(h.mb[MB_ORE_RDR] == 'A');      // the unread byte survived
    CHECK(!h.last.usart1Ore);
    CHECK(h.last.usart1Rxne == false);   // RDR was read at the end

    // ---------------------------------------------------------------
    std::printf("\n--- Test 7: 15th competition style protocol ---\n");
    CHECK(h.setPhase(0));  // back to the default 9600 configuration
    const uint32_t repliesBefore = h.mb[MB_REPLIES];
    h.sendLine("(48,92)");
    CHECK(h.waitForText("Got it"));
    h.refresh();
    CHECK(h.mb[MB_X] == 48u && h.mb[MB_Y] == 92u);
    h.sendLine("?");
    CHECK(h.waitForText("Idle"));
    h.sendLine("#");
    CHECK(h.waitForText("(48,92)"));
    h.refresh();
    std::printf("  replies %u -> %u, stored coordinates (%u,%u), baud %u\n",
                repliesBefore, h.mb[MB_REPLIES], h.mb[MB_X], h.mb[MB_Y],
                h.last.usart1Baud);
    CHECK(h.mb[MB_REPLIES] >= repliesBefore + 3u);
    CHECK(h.last.usart1Baud == 9600u);  // phase 0 restored the 9600 BRR
    CHECK(h.mb[MB_TXE_TIMEOUTS] == 0u);

    h.refresh();
    std::printf("\nusart_selftest summary: cycles=%llu (%.2f s virtual) "
                "wall=%lld ms, terminal saw %zu byte(s)\n",
                (unsigned long long)h.last.cycles,
                double(h.last.cycles) / 80.0e6, (long long)wall.elapsed(),
                h.mcuBytes);

    if (g_failures == 0) {
        std::printf("usart_selftest: PASS\n");
        return 0;
    }
    std::printf("usart_selftest: %d failure(s)\n", g_failures);
    return 1;
}