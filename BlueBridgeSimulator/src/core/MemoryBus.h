#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/IBusDevice.h"

// ============================================================================
// Memory bus: routes CPU peripheral accesses (0x40000000+) to device models.
// Unmapped addresses inside the peripheral space behave as plain storage
// (write-then-read-back keeps firmware that touches unimplemented
// peripherals alive) and are reported once through the log sink.
// ============================================================================
class MemoryBus {
public:
    struct Region {
        uint32_t base;
        uint32_t size;
        IBusDevice* device;
    };

    void addRegion(uint32_t base, uint32_t size, IBusDevice* device);

    uint32_t read(uint32_t addr, uint32_t size);
    void write(uint32_t addr, uint32_t value, uint32_t size);

    void setLogger(std::function<void(const std::string&)> logger) {
        logger_ = std::move(logger);
    }

private:
    IBusDevice* findDevice(uint32_t addr);

    std::vector<Region> regions_;
    // scratch storage for unimplemented peripheral addresses (word-keyed)
    std::unordered_map<uint32_t, uint32_t> scratch_;
    std::function<void(const std::string&)> logger_;
    uint32_t warnCount_ = 0;
};
