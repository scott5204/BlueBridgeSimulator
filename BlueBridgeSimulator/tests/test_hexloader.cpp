// Unit tests for the Intel HEX / BIN loader.
#include <cstdio>
#include <string>

#include "loader/HexLoader.h"

static int g_failed = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failed++;                                                 \
        }                                                               \
    } while (0)

// helper: compute Intel HEX checksum for a record body
static std::string record(const std::string& bodyHex) {
    // bodyHex: 2 hex chars per byte: len, addr(2), type, data..., cksum absent
    int sum = 0;
    for (size_t i = 0; i + 1 < bodyHex.size(); i += 2) {
        sum += std::stoi(bodyHex.substr(i, 2), nullptr, 16);
    }
    int ck = (-sum) & 0xFF;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02X", ck);
    return ":" + bodyHex + buf;
}

int main() {
    // ---- 1. simple data record + EOF ----
    {
        std::string hex;
        hex += record("04" "0000" "00" "01020304") + "\n";   // @0x00000000
        hex += record("00" "0000" "01") + "\n";              // EOF
        FirmwareImage img;
        std::string err;
        CHECK(HexLoader::parseIntelHexText(hex, img, err));
        CHECK(img.segments.size() == 1);
        CHECK(img.segments[0].address == 0);
        CHECK(img.segments[0].data.size() == 4);
        CHECK(img.segments[0].data[0] == 1 && img.segments[0].data[3] == 4);
        bool ok = false;
        CHECK(img.readWordLE(0, ok) == 0x04030201u && ok);
    }

    // ---- 2. extended linear address (flash at 0x08000000) ----
    {
        std::string hex;
        hex += record("02" "0000" "04" "0800") + "\n";  // ELA = 0x08000000
        hex += record("08" "0010" "00" "DEADBEEFCAFEBABE") + "\n";
        hex += record("00" "0000" "01") + "\n";
        FirmwareImage img;
        std::string err;
        CHECK(HexLoader::parseIntelHexText(hex, img, err));
        CHECK(img.segments.size() == 1);
        CHECK(img.segments[0].address == 0x08000010u);
        CHECK(img.segments[0].data.size() == 8);
        CHECK(img.segments[0].data[0] == 0xDE);
        bool ok = false;
        CHECK(img.readWordLE(0x08000010, ok) == 0xEFBEADDEu && ok);
        CHECK(img.lowestAddress() == 0x08000010u);
        CHECK(img.highestEndAddress() == 0x08000018u);
    }

    // ---- 3. contiguous records merge into one segment ----
    {
        std::string hex;
        hex += record("02" "0000" "04" "0800") + "\n";
        hex += record("04" "0000" "00" "11223344") + "\n";
        hex += record("04" "0004" "00" "55667788") + "\n";
        hex += record("00" "0000" "01") + "\n";
        FirmwareImage img;
        std::string err;
        CHECK(HexLoader::parseIntelHexText(hex, img, err));
        CHECK(img.segments.size() == 1);
        CHECK(img.segments[0].data.size() == 8);
    }

    // ---- 4. bad checksum is rejected ----
    {
        std::string hex = ":0400000001020304XX\n:00000001FF\n";
        FirmwareImage img;
        std::string err;
        CHECK(!HexLoader::parseIntelHexText(hex, img, err));
    }

    // ---- 5. missing ':' is rejected ----
    {
        std::string hex = "0400000001020304F5\n:00000001FF\n";
        FirmwareImage img;
        std::string err;
        CHECK(!HexLoader::parseIntelHexText(hex, img, err));
    }

    // ---- 6. start address record (type 05) ----
    {
        std::string hex;
        hex += record("04" "0000" "05" "08000401") + "\n";
        hex += record("04" "0000" "00" "01020304") + "\n";
        hex += record("00" "0000" "01") + "\n";
        FirmwareImage img;
        std::string err;
        CHECK(HexLoader::parseIntelHexText(hex, img, err));
        CHECK(img.hasEntry);
        CHECK(img.entryAddress == 0x08000401u);
    }

    // ---- 7. word read across segment boundary fails cleanly ----
    {
        std::string hex;
        hex += record("02" "0000" "04" "0800") + "\n";
        hex += record("02" "0000" "00" "AA55") + "\n";
        hex += record("00" "0000" "01") + "\n";
        FirmwareImage img;
        std::string err;
        CHECK(HexLoader::parseIntelHexText(hex, img, err));
        bool ok = true;
        img.readWordLE(0x08000000, ok);
        CHECK(!ok);  // only 2 bytes available
    }

    if (g_failed == 0) {
        std::printf("test_hexloader: all tests passed\n");
        return 0;
    }
    std::printf("test_hexloader: %d failure(s)\n", g_failed);
    return 1;
}
