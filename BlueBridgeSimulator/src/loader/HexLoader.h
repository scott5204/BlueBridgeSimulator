#pragma once

#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// Firmware image loaders.
//
// Supports:
//   - Intel HEX (record types 00 data / 01 EOF / 02 ext. segment /
//     04 ext. linear / 05 start address) with checksum validation
//   - raw BIN (loaded at a fixed base, default 0x08000000)
// ============================================================================
struct FirmwareImage {
    struct Segment {
        uint32_t address;
        std::vector<uint8_t> data;
    };

    std::vector<Segment> segments;
    uint32_t entryAddress = 0;   // from type-05 record, 0 if absent
    bool hasEntry = false;

    bool empty() const { return segments.empty(); }

    uint32_t lowestAddress() const;
    uint32_t highestEndAddress() const;   // one-past-the-end

    // Byte access across all segments; returns false if address not loaded.
    bool readByte(uint32_t addr, uint8_t& out) const;
    uint32_t readWordLE(uint32_t addr, bool& ok) const;
};

class HexLoader {
public:
    // Detects .hex vs .bin by file extension.
    static bool loadFile(const std::string& path, FirmwareImage& out,
                         std::string& error);

    static bool loadIntelHex(const std::string& path, FirmwareImage& out,
                             std::string& error);

    static bool loadBinary(const std::string& path, FirmwareImage& out,
                           std::string& error,
                           uint32_t baseAddress = 0x08000000u);

    // Parse from an in-memory text buffer (for tests / hex strings).
    static bool parseIntelHexText(const std::string& text, FirmwareImage& out,
                                  std::string& error);
};
