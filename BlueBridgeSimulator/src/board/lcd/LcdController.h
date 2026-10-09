#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ============================================================================
// ILI9325 LCD controller model -- CT117E-M4 on-board LCD module.
//
// It is a BOARD-level device (like the 74LS573 LED latch): it only sees the
// parallel bus pin levels that Ct117eM4 wires to the STM32 GPIOs. It knows
// nothing about the STM32 or about the firmware.
//
// Bus (verified against the official CT117E LCD BSP: lcd.h/lcd.c):
//   GPIOC[15:0] -> DB[15:0]  16-bit data bus (write and read)
//   PB5  -> NWR   write strobe, active LOW,  data latched on HIGH->LOW edge
//   PB8  -> RS    register select: 0 = command/index (IR), 1 = data (WDR)
//   PB9  -> NCS   chip select, active LOW
//   PA8  -> NRD   read strobe, active LOW (controller drives the bus)
//
//   BSP sequence per access (lcd.c LCD_WriteReg/LCD_WriteRAM):
//     CS=0, RS=cmd|data, WR=1, DB=value, WR=0 (latch), WR=1 ...
//   so a WR falling edge while NCS is low latches the current DB and RS.
//
// GRAM: 240 x 320 RGB565 (ILI9325 datasheet: "240RGBx320").
//   X = R20h "GRAM horizontal address"  (0..239)
//   Y = R21h "GRAM vertical address"    (0..319, R21h load strobes the AC)
//   R22h = write data to GRAM; every data write stores one pixel and then
//   advances the address counter (AC) following the AM / I-D[1:0] bits of
//   R03h inside the R50h..R53h window (datasheet 7.2.3 / 7.2.24).
//
// Panel mounting: the module is mounted LANDSCAPE on the board (320 x 240
// wide). GRAM (X,Y) maps to panel pixel (px,py) = (319 - Y, X):
//   - the official BSP draws its text lines along X (Line0..Line9 = 0,24,..
//     216) and its character columns along Y starting at 319 and decreasing
//     by 16, so px = 319 - Y makes the first character land on the left edge
//     and line 0 on the top edge (matches the real board / font bit0 =
//     leftmost column of the 16x24 ASCII table in fonts.h).
//
// BGR (R03h D12) only swaps the R/B order at the panel drive stage; the raw
// value written by the firmware is kept as-is (the official BSP colors like
// Red=0xF800 show as red on the real board with BGR=1).
//
// Behaviour is implemented only for what the official BSP exercises
// (see PROJECT_HANDOFF.md "LCD"). All other registers are accepted + stored
// (no behavioural effect) instead of guessing at the datasheet.
// ============================================================================
class LcdController {
public:
    // native GRAM geometry (controller addressing)
    static constexpr int kGramWidth = 240;   // X: R20h   0..239
    static constexpr int kGramHeight = 320;  // Y: R21h   0..319
    // visible panel geometry (module mounted landscape on CT117E-M4)
    static constexpr int kPanelWidth = 320;
    static constexpr int kPanelHeight = 240;

    // ---- ILI9325 register indices used by the official BSP ----
    static constexpr uint16_t kRegDriverCode = 0x0000;  // RO: device code
    static constexpr uint16_t kRegEntryMode = 0x0003;   // AM / I-D[1:0]
    static constexpr uint16_t kRegDisplayCtrl = 0x0007; // display on/off
    static constexpr uint16_t kRegAddrX = 0x0020;       // GRAM horizontal
    static constexpr uint16_t kRegAddrY = 0x0021;       // GRAM vertical
    static constexpr uint16_t kRegWriteGram = 0x0022;   // GRAM data write
    static constexpr uint16_t kRegWinXStart = 0x0050;   // HSA
    static constexpr uint16_t kRegWinXEnd = 0x0051;     // HEA
    static constexpr uint16_t kRegWinYStart = 0x0052;   // VSA
    static constexpr uint16_t kRegWinYEnd = 0x0053;     // VEA

    static constexpr uint16_t kDeviceCode = 0x9325;  // ILI9325 device code

    using LogFn = std::function<void(const std::string&)>;

    LcdController();

    // Hardware reset: registers to their datasheet defaults, GRAM cleared,
    // index register 0, address counter 0, counters cleared.
    void reset();

    // ---- parallel bus (levels exactly as seen on the module pins) ----
    // Every setter takes the ELECTRICAL pin level (true = HIGH), so the
    // board model can mirror the GPIO levels 1:1:
    void setChipSelect(bool level);      // NCS: selected while LOW
    void setRegisterSelect(bool level);  // RS: HIGH = data, LOW = command
    void setWrite(bool level);           // NWR: latches on HIGH->LOW edge
    void setRead(bool level);            // NRD: drives the bus while LOW
    void setDataBus(uint16_t value);     // DB[15:0] driven by the MCU

    // Sample the WR line level without generating an edge. Used by the board
    // right after reset, when the pin levels are not yet meaningful.
    void primeWriteLine(bool level) { wr_ = level; }

    // Value the controller drives on DB[15:0] while NRD is asserted
    // (the BSP reads GPIOC->IDR right after pulling NRD low).
    uint16_t readDataBus() const;
    // true while the controller is driving the bus (NRD low && NCS low)
    bool drivingBus() const { return !read_ && !cs_; }

    // Clocked model hook (display refresh timing is not needed for the
    // pixel-accurate GRAM; kept for interface symmetry with other devices).
    void tick(uint64_t cycles) { (void)cycles; }

    // ---- observable state (tests / GUI) ----
    uint64_t commandWriteCount() const { return commandWrites_; }
    uint64_t dataWriteCount() const { return dataWrites_; }    // non-GRAM
    uint64_t pixelWriteCount() const { return pixelWrites_; }  // GRAM pixels
    uint64_t unsupportedCommandCount() const { return unsupportedCmds_; }
    uint16_t lastCommand() const { return lastCommand_; }
    uint16_t registerValue(uint16_t index) const;

    // framebuffer dirty tracking (GUI refresh; pixel trace must not be needed)
    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

    // native GRAM pixel access (x = 0..239, y = 0..319)
    uint16_t gramPixel(int x, int y) const;
    // panel pixel access (px = 0..319, py = 0..239)
    uint16_t panelPixel(int px, int py) const;
    // panel-order RGB565 buffer (320*240), ready for the GUI
    const std::vector<uint16_t>& panelBuffer() const { return panel_; }
    void copyPanelBuffer(std::vector<uint16_t>& out) const { out = panel_; }
    // number of non-black pixels (fast sanity check for tests)
    size_t nonBlackPixelCount() const;

    // ---- tracing (development aid, off by default) ----
    // COMMAND: one line per command / non-GRAM data write.
    // PIXEL:   one line per GRAM pixel write -- never enable globally, a
    //          single LCD_Clear() produces 76800 lines.
    void setTraceCommands(bool on) { traceCommands_ = on; }
    void setTracePixels(bool on) { tracePixels_ = on; }
    void setLogger(LogFn fn) { logger_ = std::move(fn); }

private:
    void onWriteStrobe();                 // NWR falling edge, CS active
    void writeCommand(uint16_t command);
    void writeData(uint16_t value);
    void writePixel(uint16_t value);
    void advanceAddress();
    void storePixel(int x, int y, uint16_t value);
    void trace(const std::string& line) const;
    // Hard cap on traced lines per (re)initialisation. Command/pixel tracing
    // writes one GUI line AND one flushed log-file line per transaction, so an
    // untraced full-screen refresh (76k pixels) would stall the whole
    // simulation. Plenty for analysing an init sequence, and it cannot flood.
    static constexpr uint64_t kMaxTraceLines = 2000;
    mutable uint64_t traceLines_ = 0;

    // ---- bus pins (electrical levels, true = HIGH) ----
    bool cs_ = true;      // NCS (selected while low)
    bool rs_ = false;     // RS (high = data)
    bool wr_ = true;      // NWR (idle high)
    bool read_ = true;    // NRD (idle high)
    uint16_t dataBus_ = 0;

    // ---- registers / address counter ----
    uint16_t index_ = 0;       // IR (current register index)
    uint16_t lastCommand_ = 0;
    uint16_t entryMode_ = 0;   // R03h: AM (b3), I-D1 (b5), I-D0 (b4)
    uint16_t regs_[256] = {};
    bool unsupportedSeen_[256] = {};  // "unsupported command" reported once

    int x_ = 0, y_ = 0;        // address counter (AC)
    int xStart_ = 0, xEnd_ = kGramWidth - 1;
    int yStart_ = 0, yEnd_ = kGramHeight - 1;

    // ---- framebuffer (panel order 320x240, RGB565) ----
    std::vector<uint16_t> panel_;
    bool dirty_ = true;

    // ---- counters / tracing ----
    uint64_t commandWrites_ = 0;
    uint64_t dataWrites_ = 0;
    uint64_t pixelWrites_ = 0;
    uint64_t unsupportedCmds_ = 0;
    bool traceCommands_ = false;
    bool tracePixels_ = false;
    LogFn logger_;
};