#include "core/MemoryBus.h"

#include <cstdio>

IBusDevice* MemoryBus::findDevice(uint32_t addr) {
    for (auto& r : regions_) {
        if (addr >= r.base && addr < r.base + r.size) return r.device;
    }
    return nullptr;
}

void MemoryBus::addRegion(uint32_t base, uint32_t size, IBusDevice* device) {
    regions_.push_back({base, size, device});
}

uint32_t MemoryBus::read(uint32_t addr, uint32_t size) {
    if (IBusDevice* dev = findDevice(addr)) {
        return dev->busRead(addr, size);
    }
    // Unimplemented address: fall back to scratch storage so that read-back
    // of values previously written by firmware still works.
    if (warnCount_ < 64 && logger_) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "[bus] read  unimplemented @0x%08X size=%u", addr, size);
        logger_(buf);
    }
    ++warnCount_;
    auto it = scratch_.find(addr & ~uint32_t(3));
    if (it == scratch_.end()) return 0;
    uint32_t shift = (addr & 3u) * 8u;
    return (size == 1) ? ((it->second >> shift) & 0xFFu)
         : (size == 2) ? ((it->second >> shift) & 0xFFFFu)
                       : it->second;
}

void MemoryBus::write(uint32_t addr, uint32_t value, uint32_t size) {
    if (IBusDevice* dev = findDevice(addr)) {
        dev->busWrite(addr, value, size);
        return;
    }
    if (warnCount_ < 64 && logger_) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "[bus] write unimplemented @0x%08X val=0x%08X size=%u",
                      addr, value, size);
        logger_(buf);
    }
    ++warnCount_;
    const uint32_t wordAddr = addr & ~uint32_t(3);
    const uint32_t shift = (addr & 3u) * 8u;
    uint32_t mask = (size == 4) ? 0xFFFFFFFFu
                 : (size == 2) ? (0xFFFFu << shift)
                               : (0xFFu << shift);
    uint32_t word = 0;
    auto it = scratch_.find(wordAddr);
    if (it != scratch_.end()) word = it->second;
    word = (word & ~mask) | ((size == 4 ? value : value << shift) & mask);
    scratch_[wordAddr] = word;
}
