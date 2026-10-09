#pragma once

#include <cstdint>

// ============================================================================
// Memory-mapped bus device interface.
//
// The Cortex-M4 core talks to peripheral models exclusively through
// memory-mapped register accesses routed by the MemoryBus:
//
//     CPU  <->  MemoryBus  <->  IBusDevice (GPIO / RCC / SysTick / ...)
//
// @size is 1, 2 or 4 bytes; @value is right-aligned. The ARMv7-M bus only
// issues naturally aligned peripheral accesses in practice.
// ============================================================================
class IBusDevice {
public:
    virtual ~IBusDevice() = default;

    // addr is the absolute address (device decodes its own base).
    virtual uint32_t busRead(uint32_t addr, uint32_t size) = 0;
    virtual void busWrite(uint32_t addr, uint32_t value, uint32_t size) = 0;
};

// ============================================================================
// Helper base class for peripherals whose registers are 32-bit aligned words.
// The device base address is supplied at construction; busRead/busWrite
// translate absolute CPU addresses into block-relative offsets (regOff) before
// calling readReg/writeReg. Sub-word accesses are decomposed into
// read-modify-write sequences.
// ============================================================================
class BusDevice32 : public IBusDevice {
public:
    explicit BusDevice32(uint32_t base) : base_(base) {}

    uint32_t busRead(uint32_t addr, uint32_t size) override {
        const uint32_t off = addr - base_;
        const uint32_t word = readReg(off & ~uint32_t(3));
        const uint32_t shift = (off & 3u) * 8u;
        switch (size) {
        case 1: return (word >> shift) & 0xFFu;
        case 2: return (word >> shift) & 0xFFFFu;
        default: return word;
        }
    }

    void busWrite(uint32_t addr, uint32_t value, uint32_t size) override {
        const uint32_t off = addr - base_;
        const uint32_t reg = off & ~uint32_t(3);
        const uint32_t shift = (off & 3u) * 8u;
        if (size == 4) {
            writeReg(reg, value);
            return;
        }
        const uint32_t mask =
            (size == 2 ? 0xFFFFu : 0xFFu) << shift;
        const uint32_t merged =
            (readReg(reg) & ~mask) | ((value << shift) & mask);
        writeReg(reg, merged);
    }

protected:
    virtual uint32_t readReg(uint32_t regOff) = 0;
    virtual void writeReg(uint32_t regOff, uint32_t value) = 0;

public:
    // base address of the register block (used by the SoC to register regions)
    uint32_t base() const { return base_; }

private:
    uint32_t base_;
};
