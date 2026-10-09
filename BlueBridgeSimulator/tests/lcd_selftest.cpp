// Headless LCD acceptance test (three phases, exit code 0 = pass).
//
// Phase A -- LcdController device model, driven at bus level with the exact
//   GPIO sequence the official BSP produces (CS/RS/WR/DB + R20h/R21h/R22h):
//     * controller detection read (device code over the data bus)
//     * a 16 pixel row write lands at (X, Y-i) -- the BSP's character cell
//     * a sequential 76800 pixel write (LCD_Clear pattern) covers EVERY GRAM
//       cell exactly once (bijective coverage of the 240x320 window)
//
// Phase B -- end-to-end: run firmware/test_lcd/test_lcd.hex on the emulated
//   Cortex-M4 and verify the panel content that the firmware produced through
//   real ARM instructions -> GPIO MMIO -> LCD bus -> GRAM:
//     * controller code read back by the BSP over the bus
//     * exact GRAM pixel write count (76800 clear + 4800 blocks + 6912 text)
//     * colour blocks at their panel coordinates
//     * white "BlueBridge" band / red "LCD TEST" band / black background
//     * space cell of "LCD TEST" is blank (font table came from the firmware)
//   A PNG of the panel is written next to the executable for visual review.
//
// Phase C -- Qt side: render gui/LcdWidget with that framebuffer (the exact
//   widget the GUI shows) and check the RGB565 -> RGB888 conversion, the
//   aspect-preserving fit scaling (fills the widget, nearest neighbour) and
//   the centring. Writes lcd_widget.png.
//
// The test only reads the framebuffer through Simulator::copyLcdFramebuffer()
// -- the same path the GUI uses; it never reaches into the board model.
#include <QApplication>
#include <QElapsedTimer>
#include <QImage>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "board/lcd/LcdController.h"
#include "gui/LcdWidget.h"
#include "sim/Simulator.h"

static int g_failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                               \
        }                                                               \
    } while (0)

// firmware self-test mailbox (linker script places .selfdata at 0x20000000)
static constexpr uint32_t kMailBase = 0x20000000u;
static constexpr uint32_t kMailMagic = 0x1CD00001u;

// panel geometry (module mounted landscape on CT117E-M4)
static constexpr int kPanelW = LcdController::kPanelWidth;   // 320
static constexpr int kPanelH = LcdController::kPanelHeight;  // 240

inline uint16_t panelPx(const std::vector<uint16_t>& panel, int px, int py) {
    return panel[size_t(py) * kPanelW + px];
}

inline size_t countColorInBand(const std::vector<uint16_t>& panel, int py0,
                               int py1, int px0, int px1, uint16_t color) {
    size_t n = 0;
    for (int py = py0; py <= py1; py++)
        for (int px = px0; px <= px1; px++)
            if (panelPx(panel, px, py) == color) ++n;
    return n;
}

inline size_t countNonBlack(const std::vector<uint16_t>& panel) {
    size_t n = 0;
    for (uint16_t p : panel)
        if (p != 0x0000) ++n;
    return n;
}

void savePng(const std::vector<uint16_t>& panel, const char* path) {
    QImage img(kPanelW, kPanelH, QImage::Format_RGB888);
    for (int py = 0; py < kPanelH; py++) {
        uchar* out = img.scanLine(py);
        for (int px = 0; px < kPanelW; px++) {
            const uint16_t p = panelPx(panel, px, py);
            const uint32_t r5 = (p >> 11) & 0x1Fu, g6 = (p >> 5) & 0x3Fu,
                           b5 = p & 0x1Fu;
            *out++ = uchar((r5 << 3) | (r5 >> 2));
            *out++ = uchar((g6 << 2) | (g6 >> 4));
            *out++ = uchar((b5 << 3) | (b5 >> 2));
        }
    }
    if (img.save(path))
        std::printf("phase B: panel screenshot written to %s\n", path);
    else
        std::printf("phase B: could not write %s\n", path);
}

// ---------------------------------------------------------------------------
// Phase A: bus-level device model checks (BSP timing, no emulator involved)
// ---------------------------------------------------------------------------
namespace {

// Reproduces the exact pin-level sequence of lcd.c LCD_WriteReg /
// LCD_WriteRAM_Prepare / LCD_WriteRAM / LCD_ReadReg.
struct LcdBus {
    LcdController& lcd;

    void command(uint16_t value) {
        lcd.setChipSelect(false);      // NCS low
        lcd.setRegisterSelect(false);  // RS = command
        lcd.setWrite(true);            // NWR high
        lcd.setDataBus(value);
        lcd.setWrite(false);           // falling edge latches the command
        lcd.setWrite(true);
    }
    void data(uint16_t value) {
        lcd.setChipSelect(false);
        lcd.setRegisterSelect(true);  // RS = data
        lcd.setWrite(true);
        lcd.setDataBus(value);
        lcd.setWrite(false);          // falling edge latches the data
        lcd.setWrite(true);
        lcd.setChipSelect(true);
    }
    void writeReg(uint16_t index, uint16_t value) {
        command(index);
        data(value);
    }
    void writeRamPrepare() {
        command(0x0022u);
        lcd.setChipSelect(true);
    }
    void writeRam(uint16_t pixel) { data(pixel); }

    uint16_t readReg(uint16_t index) {
        command(index);
        lcd.setRegisterSelect(true);
        lcd.setRead(false);  // NRD low: the controller drives the bus
        const uint16_t v = lcd.readDataBus();
        lcd.setRead(true);
        lcd.setChipSelect(true);
        return v;
    }
};

void phaseA() {
    std::printf("---- phase A: LcdController bus / GRAM model ----\n");
    LcdController lcd;
    LcdBus bus{lcd};

    CHECK(lcd.gramPixel(0, 0) == 0);
    CHECK(lcd.commandWriteCount() == 0);

    // controller detection, exactly like LCD_Init()/LCD_ReadReg(0)
    CHECK(bus.readReg(0x0000) == LcdController::kDeviceCode);

    // BSP entry mode: AM=1 (vertical update) + I/D=01 (V decrement) ->
    // R03h = 0x1018; window = whole GRAM (0..239 x 0..319), as REG_932X_Init
    bus.writeReg(0x0003, 0x1018);
    bus.writeReg(0x0050, 0x0000);
    bus.writeReg(0x0051, 0x00EF);
    bus.writeReg(0x0052, 0x0000);
    bus.writeReg(0x0053, 0x013F);

    // 16 pixel row write (LCD_DrawChar inner loop): cursor (10,100) then 16
    // consecutive writes must land on GRAM (10, 100-i)
    bus.writeReg(0x0020, 0x000A);
    bus.writeReg(0x0021, 0x0064);
    bus.writeRamPrepare();
    for (uint16_t i = 0; i < 16; i++) bus.writeRam(uint16_t(0x1000 + i));
    for (uint16_t i = 0; i < 16; i++)
        CHECK(lcd.gramPixel(10, 100 - int(i)) == uint16_t(0x1000 + i));
    CHECK(lcd.gramPixel(10, 101) == 0);

    // sequential whole-GRAM write (LCD_Clear pattern): every cell exactly
    // once. Every write uses a non-zero marker, so "all 76800 cells are
    // non-zero after exactly 76800 writes" is a bijection proof (pigeonhole).
    const int cells = LcdController::kGramWidth * LcdController::kGramHeight;
    lcd.reset();
    bus.writeReg(0x0003, 0x1018);
    bus.writeReg(0x0020, 0x0000);
    bus.writeReg(0x0021, 0x0000);
    bus.writeRamPrepare();
    for (int i = 0; i < cells; i++)
        bus.writeRam(uint16_t((i % 0xFFFE) + 1));  // never 0
    size_t written = 0;
    for (int y = 0; y < LcdController::kGramHeight; y++) {
        for (int x = 0; x < LcdController::kGramWidth; x++) {
            if (lcd.gramPixel(x, y) != 0) ++written;
        }
    }
    CHECK(written == size_t(cells));
    CHECK(lcd.pixelWriteCount() == uint64_t(cells));
    std::printf(
        "phase A: sequential 76800-pixel write covered every GRAM cell "
        "(written=%zu)\n",
        written);
}

// ---------------------------------------------------------------------------
// Phase B: firmware end to end
// ---------------------------------------------------------------------------
struct Ctx {
    SimSnapshot last;
    bool fwLoaded = false;
    bool loadFailed = false;
    bool faulted = false;
};

bool readMail(Simulator& sim, int word, uint32_t& out) {
    return sim.debugSramWord(kMailBase + 4u * uint32_t(word), out);
}

void phaseB(const char* hexPath, std::vector<uint16_t>& panelOut) {
    std::printf("---- phase B: firmware end-to-end (%s) ----\n", hexPath);
    Simulator sim;
    Ctx ctx;

    QObject::connect(&sim, &Simulator::stateChanged,
                     [&](const SimSnapshot& s) { ctx.last = s; });
    QObject::connect(&sim, &Simulator::firmwareLoadedSig,
                     [&](const QString&) { ctx.fwLoaded = true; });
    QObject::connect(&sim, &Simulator::firmwareLoadFailed,
                     [&](const QString& e) {
                         ctx.loadFailed = true;
                         std::printf("load failed: %s\n",
                                     e.toUtf8().constData());
                     });
    QObject::connect(&sim, &Simulator::faulted, [&](const QString& e) {
        ctx.faulted = true;
        std::printf("fault: %s\n", e.toUtf8().constData());
    });
    // LCD command / unsupported-register trace (development aid, see
    // PROJECT_HANDOFF "LCD trace"). Pixel tracing stays off: one LCD_Clear
    // would produce 76800 lines.
    QObject::connect(&sim, &Simulator::logMessage, [](const QString& m) {
        std::printf("log: %s\n", m.toUtf8().constData());
    });
    sim.setLcdTrace(true, false);  // command/data trace into the log

    sim.loadFirmware(QString::fromLocal8Bit(hexPath));
    CHECK(ctx.fwLoaded);
    CHECK(!ctx.loadFailed);
    if (g_failures) return;

    // ---- run until the firmware finished drawing (mailbox stage == 4) ----
    QElapsedTimer wall;
    wall.start();
    uint32_t stage = 0;
    int steps = 0;
    while (wall.elapsed() < 60000) {
        sim.slotStep(20);
        ++steps;
        if (ctx.faulted) break;
        readMail(sim, 3, stage);
        if (stage >= 4) break;
    }
    uint32_t magic = 0, tick = 0, loop = 0, devid = 0, cfgr = 0;
    readMail(sim, 0, magic);
    readMail(sim, 1, tick);
    readMail(sim, 2, loop);
    readMail(sim, 4, devid);
    readMail(sim, 5, cfgr);
    std::printf(
        "phase B: steps=%d cycles=%llu stage=%u magic=0x%08X devid=0x%04X "
        "cfgr=0x%08X uwTick=%u loop=%u\n",
        steps, (unsigned long long)ctx.last.cycles, stage, magic, devid, cfgr,
        tick, loop);

    CHECK(!ctx.faulted);
    CHECK(magic == kMailMagic);
    CHECK(stage == 4);       // all drawing finished
    CHECK(devid == 0x9325);  // controller code read over the GPIO bus
    CHECK(ctx.last.sysclkHz == 80'000'000u);
    CHECK((cfgr & 0xCu) == 0xCu);  // SYSCLK switched to the PLL
    std::printf("phase B: GPIO at end: B=0x%04X A=0x%04X C(odr)=0x%04X\n",
                ctx.last.gpioB_Idr & 0xFFFF, ctx.last.gpioA_Idr & 0xFFFF,
                ctx.last.gpioC_Odr & 0xFFFF);

    // ---- exact GRAM pixel write count (== real bus pixel writes) ----
    // 76800 LCD_Clear + 5*40*24 colour blocks + (10+8)*24*16 font pixels
    const uint64_t expectedPixels =
        76800u + 5u * 40u * 24u + (10u + 8u) * 24u * 16u;
    std::printf("phase B: pixel writes=%llu (expected %llu), commands=%llu\n",
                (unsigned long long)ctx.last.lcdPixelWrites,
                (unsigned long long)expectedPixels,
                (unsigned long long)ctx.last.lcdCommandWrites);
    CHECK(ctx.last.lcdPixelWrites == expectedPixels);
    CHECK(ctx.last.lcdCommandWrites > 500);  // init + per-row R22h prepares

    // ---- framebuffer content (through the same copy the GUI uses) ----
    std::vector<uint16_t>& panel = panelOut;
    sim.copyLcdFramebuffer(panel);
    CHECK(panel.size() == size_t(kPanelW) * kPanelH);

    savePng(panel, "lcd_panel.png");

    // colour blocks: 40x24 at panel row 100..123, x = 40/100/160/220/280
    CHECK(panelPx(panel, 40, 100) == 0xF800);   // Red
    CHECK(panelPx(panel, 100, 100) == 0x07E0);  // Green
    CHECK(panelPx(panel, 160, 100) == 0x001F);  // Blue
    CHECK(panelPx(panel, 220, 100) == 0xFFFF);  // White
    CHECK(panelPx(panel, 280, 100) == 0x7FFF);  // Cyan
    CHECK(panelPx(panel, 79, 123) == 0xF800);   // block bottom right corner
    CHECK(panelPx(panel, 39, 100) == 0x0000);   // gap before the blocks
    CHECK(panelPx(panel, 80, 100) == 0x0000);
    CHECK(panelPx(panel, 40, 99) == 0x0000);    // row above the blocks
    CHECK(panelPx(panel, 319, 239) == 0x0000);  // untouched corner

    // text bands: line 1 white, line 2 red. The glyph pixels come from the
    // firmware's own ASCII_Table, nothing is rendered on the host side.
    const size_t white1 =
        countColorInBand(panel, 24, 47, 0, kPanelW - 1, 0xFFFF);
    const size_t red2 =
        countColorInBand(panel, 48, 71, 0, kPanelW - 1, 0xF800);
    const size_t white2 =
        countColorInBand(panel, 48, 71, 0, kPanelW - 1, 0xFFFF);
    std::printf("phase B: text pixels: line1 white=%zu, line2 red=%zu\n",
                white1, red2);
    CHECK(white1 > 100);
    CHECK(red2 > 100);
    CHECK(white2 == 0);

    // character 3 of "LCD TEST" is a space: its 16x24 cell must be blank
    CHECK(countColorInBand(panel, 48, 71, 48, 63, 0xF800) == 0);

    // background between the content stays black (cleared screen)
    CHECK(countColorInBand(panel, 72, 95, 0, kPanelW - 1, 0x0000) ==
          size_t(kPanelW) * 24);
    const size_t nonBlack = countNonBlack(panel);
    std::printf("phase B: non-black pixels=%zu\n", nonBlack);
    // 5 colour blocks = 4800 pixels + the white/red glyph pixels
    CHECK(nonBlack > 5000);

    // ---- reset returns the controller to its reset state ----
    sim.slotReset();
    std::vector<uint16_t> cleared;
    sim.copyLcdFramebuffer(cleared);
    CHECK(countNonBlack(cleared) == 0);
}

// ---------------------------------------------------------------------------
// Phase C: Qt widget rendering (the exact widget the GUI shows)
// ---------------------------------------------------------------------------
QImage renderWidget(LcdWidget& w, int width, int height) {
    w.resize(width, height);
    QImage shot(width, height, QImage::Format_RGB888);
    shot.fill(QColor(0xDE, 0xAD, 0xBE));  // marker: anything not painted
    w.render(&shot);
    return shot;
}

QColor at(const QImage& img, int x, int y) { return QColor(img.pixel(x, y)); }

// Panel placement computed exactly like LcdWidget::paintEvent: aspect ratio
// preserved, scaled to the largest size that fits, centred.
struct Fit {
    double scale = 1.0;
    int dw = 0, dh = 0, dx = 0, dy = 0;
};

Fit fitFor(int w, int h) {
    Fit f;
    f.scale = std::min(double(w) / kPanelW, double(h) / kPanelH);
    f.dw = std::max(1, int(kPanelW * f.scale));
    f.dh = std::max(1, int(kPanelH * f.scale));
    f.dx = (w - f.dw) / 2;
    f.dy = (h - f.dh) / 2;
    return f;
}

// widget point at the centre of a source (panel) pixel
QPoint atPanel(const Fit& f, int px, int py) {
    return QPoint(f.dx + int((px + 0.5) * f.scale),
                  f.dy + int((py + 0.5) * f.scale));
}

void phaseC(const std::vector<uint16_t>& panel) {
    std::printf("---- phase C: Qt LcdWidget offscreen render ----\n");
    if (panel.size() != size_t(kPanelW) * kPanelH) {
        CHECK(false);
        return;
    }

    LcdWidget widget;
    widget.setFramebuffer(panel);

    // (1) exactly 2x widget: the panel fills it completely, one quad per pixel
    // RGB565 0xF800 -> RGB888 (255,0,0) by bit replication
    const Fit f2 = fitFor(kPanelW * 2, kPanelH * 2);
    CHECK(f2.dw == kPanelW * 2 && f2.dh == kPanelH * 2);
    const QImage shot2x = renderWidget(widget, kPanelW * 2, kPanelH * 2);
    CHECK(at(shot2x, 80, 200) == QColor(255, 0, 0));  // red block, 2x
    CHECK(at(shot2x, 159, 247) == QColor(255, 0, 0));
    CHECK(at(shot2x, 78, 200) == QColor(0, 0, 0));  // panel black
    CHECK(at(shot2x, 2 * 220 + 1, 2 * 100 + 1) == QColor(255, 255, 255));
    // white "BlueBridge" glyph pixels are present in the 2x render
    size_t white = 0;
    for (int y = 2 * 24; y < 2 * 48; y++)
        for (int x = 0; x < 2 * kPanelW; x++)
            if (at(shot2x, x, y) == QColor(0xFF, 0xFF, 0xFF)) ++white;
    std::printf("phase C: white pixels in the 2x line-1 band: %zu\n", white);
    CHECK(white > 1500);

    // (2) widget size that is NOT a multiple of the panel: it must still fill
    // the widget (no integer-only margin), keep the 320:240 aspect, centre the
    // remainder and stay sharp (nearest neighbour).
    const int w2 = 500, h2 = 400;
    const Fit f1 = fitFor(w2, h2);
    const QImage shot1x = renderWidget(widget, w2, h2);
    std::printf(
        "phase C: 500x400 widget -> panel %dx%d at (%d,%d), scale=%.3f\n",
        f1.dw, f1.dh, f1.dx, f1.dy, f1.scale);
    CHECK(f1.dw == 500);  // fills the width (the smaller ratio)
    CHECK(f1.dh > kPanelH);  // bigger than the 1x panel would have been
    CHECK(std::abs(double(f1.dw) / f1.dh - double(kPanelW) / kPanelH) < 0.02);
    const QPoint red = atPanel(f1, 40 + 20, 100 + 12);  // inside the red block
    CHECK(at(shot1x, red.x(), red.y()) == QColor(255, 0, 0));
    const QPoint blank = atPanel(f1, 20, 200);  // cleared screen area
    CHECK(at(shot1x, blank.x(), blank.y()) == QColor(0, 0, 0));
    // outside the panel: the widget's own bezel colour, not the marker
    CHECK(at(shot1x, 0, 0) == QColor(0x16, 0x21, 0x3e));
    CHECK(at(shot1x, w2 - 1, h2 - 1) == QColor(0x16, 0x21, 0x3e));

    if (shot1x.save("lcd_widget.png")) {
        std::printf("phase C: widget screenshot written to lcd_widget.png\n");
    } else {
        std::printf("phase C: could not write lcd_widget.png\n");
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    qRegisterMetaType<SimSnapshot>("SimSnapshot");

    if (argc < 2) {
        std::printf("usage: lcd_selftest <test_lcd.hex>\n");
        return 2;
    }

    phaseA();
    std::vector<uint16_t> panel;
    if (g_failures == 0) phaseB(argv[1], panel);
    if (g_failures == 0) phaseC(panel);

    if (g_failures == 0) {
        std::printf("lcd_selftest: PASS\n");
        return 0;
    }
    std::printf("lcd_selftest: %d failure(s)\n", g_failures);
    return 1;
}