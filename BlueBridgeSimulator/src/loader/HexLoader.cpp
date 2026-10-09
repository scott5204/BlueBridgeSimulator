#include "loader/HexLoader.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>

namespace {

bool readWholeFile(const std::string& path, std::string& out,
                   std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open file: " + path;
        return false;
    }
    f.seekg(0, std::ios::end);
    std::streampos len = f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(len));
    if (len > 0) f.read(&out[0], len);
    return true;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool isBlankLine(const std::string& s) {
    for (char c : s) {
        if (!std::isspace(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

}  // namespace

uint32_t FirmwareImage::lowestAddress() const {
    uint32_t lo = 0xFFFFFFFFu;
    for (auto& s : segments) lo = std::min(lo, s.address);
    return lo;
}

uint32_t FirmwareImage::highestEndAddress() const {
    uint32_t hi = 0;
    for (auto& s : segments)
        hi = std::max(hi, s.address + static_cast<uint32_t>(s.data.size()));
    return hi;
}

bool FirmwareImage::readByte(uint32_t addr, uint8_t& out) const {
    for (auto& s : segments) {
        if (addr >= s.address &&
            addr < s.address + static_cast<uint32_t>(s.data.size())) {
            out = s.data[addr - s.address];
            return true;
        }
    }
    return false;
}

uint32_t FirmwareImage::readWordLE(uint32_t addr, bool& ok) const {
    ok = false;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t b;
        if (!readByte(addr + static_cast<uint32_t>(i), b)) return 0;
        v |= static_cast<uint32_t>(b) << (8 * i);
    }
    ok = true;
    return v;
}

bool HexLoader::loadFile(const std::string& path, FirmwareImage& out,
                         std::string& error) {
    auto dot = path.find_last_of('.');
    std::string ext = (dot == std::string::npos)
                          ? ""
                          : path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == "bin") {
        return loadBinary(path, out, error);
    }
    // default: treat as Intel HEX
    return loadIntelHex(path, out, error);
}

bool HexLoader::loadIntelHex(const std::string& path, FirmwareImage& out,
                             std::string& error) {
    std::string text;
    if (!readWholeFile(path, text, error)) return false;
    return parseIntelHexText(text, out, error);
}

bool HexLoader::loadBinary(const std::string& path, FirmwareImage& out,
                           std::string& error, uint32_t baseAddress) {
    std::string data;
    if (!readWholeFile(path, data, error)) return false;
    if (data.empty()) {
        error = "binary file is empty";
        return false;
    }
    FirmwareImage::Segment seg;
    seg.address = baseAddress;
    seg.data.assign(data.begin(), data.end());
    out.segments.push_back(std::move(seg));
    return true;
}

bool HexLoader::parseIntelHexText(const std::string& text, FirmwareImage& out,
                                  std::string& error) {
    out = FirmwareImage{};
    uint32_t baseAddress = 0;          // from extended records
    size_t lineNo = 0;
    bool eof = false;

    std::string line;
    line.reserve(600);
    for (size_t i = 0; i <= text.size(); i++) {
        bool flush = (i == text.size());
        if (!flush) {
            char c = text[i];
            if (c != '\n' && c != '\r') {
                if (line.size() < 600) line.push_back(c);
                continue;
            }
            if (line.empty()) continue;   // skip empty separators
        }
        lineNo++;
        if (eof && !isBlankLine(line)) {
            error = "record after EOF at line " + std::to_string(lineNo);
            return false;
        }
        if (isBlankLine(line)) {
            line.clear();
            continue;
        }
        if (line[0] != ':') {
            error = "missing ':' at line " + std::to_string(lineNo);
            return false;
        }
        // parse hex pairs
        std::vector<uint8_t> bytes;
        bytes.reserve(64);
        bool bad = false;
        for (size_t k = 1; k + 1 < line.size(); k += 2) {
            int hi = hexDigit(line[k]);
            int lo = hexDigit(line[k + 1]);
            if (hi < 0 || lo < 0) {
                bad = true;
                break;
            }
            bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
        }
        if (bad || bytes.size() < 5) {
            error = "malformed record at line " + std::to_string(lineNo);
            return false;
        }
        // checksum: two's complement of sum of all bytes
        uint8_t sum = 0;
        for (uint8_t b : bytes) sum = uint8_t(sum + b);
        if (sum != 0) {
            error = "checksum error at line " + std::to_string(lineNo);
            return false;
        }
        uint8_t count = bytes[0];
        uint16_t offset = uint16_t((bytes[1] << 8) | bytes[2]);
        uint8_t type = bytes[3];
        if (bytes.size() != size_t(count) + 5) {
            error = "length mismatch at line " + std::to_string(lineNo);
            return false;
        }
        const uint8_t* data = bytes.data() + 4;

        switch (type) {
        case 0x00: {  // data
            if (count == 0) break;
            uint32_t absAddr = baseAddress + offset;
            if (!out.segments.empty() &&
                out.segments.back().address +
                        out.segments.back().data.size() ==
                    absAddr) {
                out.segments.back().data.insert(out.segments.back().data.end(),
                                                data, data + count);
            } else {
                FirmwareImage::Segment seg;
                seg.address = absAddr;
                seg.data.assign(data, data + count);
                out.segments.push_back(std::move(seg));
            }
            break;
        }
        case 0x01:  // EOF
            eof = true;
            break;
        case 0x02: {  // extended segment address
            uint32_t seg = uint32_t((data[0] << 8) | data[1]) << 4;
            baseAddress = seg;
            break;
        }
        case 0x04: {  // extended linear address
            uint32_t hi = uint32_t((data[0] << 8) | data[1]) << 16;
            baseAddress = hi;
            break;
        }
        case 0x05: {  // start linear address
            out.entryAddress = uint32_t((uint32_t(data[0]) << 24) |
                                        (uint32_t(data[1]) << 16) |
                                        (uint32_t(data[2]) << 8) |
                                        uint32_t(data[3]));
            out.hasEntry = true;
            break;
        }
        default:
            break;  // unknown record types are ignored
        }
        line.clear();
    }

    if (!eof) {
        // Some toolchains omit the EOF record; tolerate but note it.
        // (Keil/ARMCC always emits it.)
    }
    if (out.segments.empty()) {
        error = "no data records found";
        return false;
    }
    return true;
}
