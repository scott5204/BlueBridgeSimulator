#pragma once

#include <cstdint>
#include <functional>

#include "core/IBusDevice.h"

// ============================================================================
// DWT (Data Watchpoint and Trace) at 0xE0001000.
// Only the cycle counter is modelled (some delay libraries use DWT->CYCCNT).
//   CTRL 0x00 (bit0 CYCCNTENA), CYCCNT 0x04
// ============================================================================
class DwtDevice : public BusDevice32 {
public:
    static constexpr uint32_t kBase = 0xE0001000u;
    static constexpr uint32_t kSize = 0x1000u;

    DwtDevice() : BusDevice32(kBase) {}
    void reset() { ctrl_ = 0; }
    // Simulator feeds the virtual cycle counter
    std::function<uint64_t()> cycleProvider;

protected:
    uint32_t readReg(uint32_t regOff) override {
        switch (regOff) {
        case 0x00: return ctrl_ | 1u;  // NUMCMT = 1
        case 0x04:
            if ((ctrl_ & 1u) && cycleProvider) {
                return uint32_t(cycleProvider());
            }
            return 0;
        default: return 0;
        }
    }
    void writeReg(uint32_t regOff, uint32_t value) override {
        if (regOff == 0x00) ctrl_ = value & 1u;
        // CYCCNT writes are ignored (the virtual clock is authoritative)
    }

private:
    uint32_t ctrl_ = 0;
};

// ============================================================================
// ITM (Instrumentation Trace Macrocell) at 0xE0000000.
// Stimulus port writes (Keil "Debug (printf) Viewer") are forwarded to the
// simulator log so firmware printf debugging works out of the box.
// ============================================================================
class ItmDevice : public BusDevice32 {
public:
    static constexpr uint32_t kBase = 0xE0000000u;
    static constexpr uint32_t kSize = 0x1000u;

    ItmDevice() : BusDevice32(kBase) {}
    void reset() {}
    std::function<void(const char* /*text*/)> textSink;

protected:
    uint32_t readReg(uint32_t) override { return 0; }
    void writeReg(uint32_t regOff, uint32_t value) override {
        if (regOff < 0x100) {
            // stimulus port 0..31, byte lanes carry characters
            char buf[5];
            int n = 0;
            for (int i = 0; i < 4; i++) {
                char c = char((value >> (8 * i)) & 0xFFu);
                if (c >= 0x20 || c == '\n' || c == '\r' || c == '\t')
                    buf[n++] = c;
            }
            buf[n] = 0;
            if (n > 0 && textSink) textSink(buf);
        }
    }
};
