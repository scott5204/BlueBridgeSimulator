#include "board/lcd/LcdController.h"

#include <cstdio>

namespace {
// entry mode R03h bits (ILI9325 datasheet 7.2.3)
constexpr uint16_t kEntryAm = 1u << 3;   // 0 = update horizontal, 1 = vertical
constexpr uint16_t kEntryId0 = 1u << 4;  // horizontal direction (+1 / -1)
constexpr uint16_t kEntryId1 = 1u << 5;  // vertical direction (+1 / -1)

constexpr uint16_t kDefaultEntryMode = 0x1030;  // ILI9325 reset value

inline int stepCounter(int v, bool increment, int lo, int hi) {
    if (increment) return (v + 1 > hi) ? lo : v + 1;
    return (v - 1 < lo) ? hi : v - 1;
}

std::string toHex4(uint16_t v) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04X", unsigned(v));
    return buf;
}
}  // namespace

LcdController::LcdController() {
    reset();
}

void LcdController::reset() {
    cs_ = true;
    rs_ = false;
    wr_ = true;
    read_ = true;
    dataBus_ = 0;

    for (uint16_t& r : regs_) r = 0;
    for (bool& b : unsupportedSeen_) b = false;
    traceLines_ = 0;  // fresh trace budget per reset (see kMaxTraceLines)
    index_ = 0;
    lastCommand_ = 0;
    entryMode_ = kDefaultEntryMode;

    xStart_ = 0;
    xEnd_ = kGramWidth - 1;
    yStart_ = 0;
    yEnd_ = kGramHeight - 1;
    x_ = 0;
    y_ = 0;

    regs_[kRegEntryMode] = entryMode_;
    regs_[kRegAddrX] = 0;
    regs_[kRegAddrY] = 0;
    regs_[kRegWinXStart] = uint16_t(xStart_);
    regs_[kRegWinXEnd] = uint16_t(xEnd_);
    regs_[kRegWinYStart] = uint16_t(yStart_);
    regs_[kRegWinYEnd] = uint16_t(yEnd_);

    panel_.assign(size_t(kPanelWidth) * kPanelHeight, 0x0000);
    dirty_ = true;

    commandWrites_ = 0;
    dataWrites_ = 0;
    pixelWrites_ = 0;
    unsupportedCmds_ = 0;
}

// ---------------------------------------------------------------------------
// parallel bus
// ---------------------------------------------------------------------------
void LcdController::setChipSelect(bool level) { cs_ = level; }

void LcdController::setRegisterSelect(bool level) { rs_ = level; }

void LcdController::setDataBus(uint16_t value) { dataBus_ = value; }

void LcdController::setWrite(bool level) {
    const bool fallingEdge = (wr_ && !level);
    wr_ = level;
    // The module latches the bus on the NWR HIGH->LOW edge while selected.
    if (fallingEdge && !cs_) onWriteStrobe();
}

void LcdController::setRead(bool level) { read_ = level; }

uint16_t LcdController::readDataBus() const {
    if (!drivingBus()) return 0;
    if (index_ == kRegDriverCode) return kDeviceCode;
    if (index_ == kRegWriteGram) return gramPixel(x_, y_);  // GRAM read
    return regs_[index_ & 0xFFu];
}

// ---------------------------------------------------------------------------
// register / GRAM access
// ---------------------------------------------------------------------------
void LcdController::onWriteStrobe() {
    if (rs_)
        writeData(dataBus_);
    else
        writeCommand(dataBus_);
}

void LcdController::writeCommand(uint16_t command) {
    index_ = command;
    lastCommand_ = command;
    ++commandWrites_;

    switch (command) {
    // Registers with behaviour in this model: they act on their data write
    // (R20h/R21h address counter, R50h..R53h window) or select the GRAM
    // access port (R22h).
    case kRegDriverCode:
    case kRegEntryMode:
    case kRegDisplayCtrl:
    case kRegAddrX:
    case kRegAddrY:
    case kRegWriteGram:
    case kRegWinXStart:
    case kRegWinXEnd:
    case kRegWinYStart:
    case kRegWinYEnd:
        break;
    default: {
        // Accepted + stored, currently no behavioural effect (never guess at
        // the datasheet -- see PROJECT_HANDOFF). Report each index once.
        ++unsupportedCmds_;
        const size_t idx = size_t(command & 0xFFu);
        if (command < 256 && !unsupportedSeen_[idx]) {
            unsupportedSeen_[idx] = true;
            trace("[LCD] Unsupported command: 0x" + toHex4(command) +
                  " (accepted + stored, no behavioural effect)");
        }
        break;
    }
    }
    if (traceCommands_) trace("[LCD] CMD 0x" + toHex4(command));
}

void LcdController::writeData(uint16_t value) {
    if (index_ == kRegWriteGram) {
        writePixel(value);
        return;
    }
    ++dataWrites_;

    switch (index_) {
    case kRegAddrX:
        x_ = (value < kGramWidth) ? int(value) : kGramWidth - 1;
        break;
    case kRegAddrY:
        // R21h load latches the address counter (datasheet 7.2.18 note 2)
        y_ = (value < kGramHeight) ? int(value) : kGramHeight - 1;
        break;
    case kRegWinXStart:
        if (int(value) < xEnd_) xStart_ = int(value);
        break;
    case kRegWinXEnd:
        if (int(value) > xStart_ && value < kGramWidth) xEnd_ = int(value);
        break;
    case kRegWinYStart:
        if (int(value) < yEnd_) yStart_ = int(value);
        break;
    case kRegWinYEnd:
        if (int(value) > yStart_ && value < kGramHeight) yEnd_ = int(value);
        break;
    case kRegEntryMode:
        entryMode_ = value;
        break;
    default:
        break;  // stored only
    }

    if (index_ < 256) {
        regs_[index_ & 0xFFu] = value;
        // keep the clamped model state visible through register read-back
        if (index_ == kRegWinXStart) regs_[index_] = uint16_t(xStart_);
        if (index_ == kRegWinXEnd) regs_[index_] = uint16_t(xEnd_);
        if (index_ == kRegWinYStart) regs_[index_] = uint16_t(yStart_);
        if (index_ == kRegWinYEnd) regs_[index_] = uint16_t(yEnd_);
    }
    if (traceCommands_)
        trace("[LCD] DATA 0x" + toHex4(value) + " (R" + toHex4(index_) + ")");
}

void LcdController::writePixel(uint16_t value) {
    storePixel(x_, y_, value);
    ++pixelWrites_;
    if (tracePixels_) {
        trace("[LCD] PIXEL x=" + std::to_string(x_) +
              " y=" + std::to_string(y_) + " rgb=0x" + toHex4(value));
    }
    advanceAddress();
}

void LcdController::storePixel(int x, int y, uint16_t value) {
    if (x < 0 || x >= kGramWidth || y < 0 || y >= kGramHeight) return;
    // GRAM (X,Y) -> landscape panel (px,py): px = 319 - Y, py = X
    const int px = (kPanelWidth - 1) - y;
    const int py = x;
    panel_[size_t(py) * kPanelWidth + px] = value;
    dirty_ = true;
}

// Address counter update after a GRAM write, following R03h AM / I-D[1:0]
// inside the R50h..R53h window. The selected writing direction is the fast
// axis; when it reaches the window edge it wraps to the opposite edge and the
// other axis steps with the remaining I-D bit (this is what makes a
// full-window sequential write -- LCD_Clear -- cover every pixel once).
void LcdController::advanceAddress() {
    const bool vertical = (entryMode_ & kEntryAm) != 0;
    const bool id0 = (entryMode_ & kEntryId0) != 0;  // horizontal +1 / -1
    const bool id1 = (entryMode_ & kEntryId1) != 0;  // vertical   +1 / -1

    if (!vertical) {
        x_ += id0 ? 1 : -1;
        if (x_ > xEnd_) {
            x_ = xStart_;
            y_ = stepCounter(y_, id1, yStart_, yEnd_);
        } else if (x_ < xStart_) {
            x_ = xEnd_;
            y_ = stepCounter(y_, id1, yStart_, yEnd_);
        }
    } else {
        y_ += id1 ? 1 : -1;
        if (y_ > yEnd_) {
            y_ = yStart_;
            x_ = stepCounter(x_, id0, xStart_, xEnd_);
        } else if (y_ < yStart_) {
            y_ = yEnd_;
            x_ = stepCounter(x_, id0, xStart_, xEnd_);
        }
    }
}

// ---------------------------------------------------------------------------
// inspection
// ---------------------------------------------------------------------------
uint16_t LcdController::registerValue(uint16_t index) const {
    if (index == kRegDriverCode) return kDeviceCode;
    return regs_[index & 0xFFu];
}

uint16_t LcdController::gramPixel(int x, int y) const {
    if (x < 0 || x >= kGramWidth || y < 0 || y >= kGramHeight) return 0;
    const int px = (kPanelWidth - 1) - y;
    return panel_[size_t(x) * kPanelWidth + px];
}

uint16_t LcdController::panelPixel(int px, int py) const {
    if (px < 0 || px >= kPanelWidth || py < 0 || py >= kPanelHeight) return 0;
    return panel_[size_t(py) * kPanelWidth + px];
}

size_t LcdController::nonBlackPixelCount() const {
    size_t n = 0;
    for (uint16_t p : panel_)
        if (p != 0x0000) ++n;
    return n;
}

void LcdController::trace(const std::string& line) const {
    if (!logger_) return;
    if (traceLines_ >= kMaxTraceLines) {
        if (traceLines_ == kMaxTraceLines) {
            traceLines_ = kMaxTraceLines + 1;
            logger_("[LCD] trace limit reached (" +
                    std::to_string(kMaxTraceLines) +
                    " lines) - further LCD tracing suppressed");
        }
        return;
    }
    ++traceLines_;
    logger_(line);
}