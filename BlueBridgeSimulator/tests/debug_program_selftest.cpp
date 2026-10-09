// ============================================================================
// Stage 7-2A acceptance test: virtual flash programming, DIRECT (no IPC).
//
// Exercises IFlashProgrammer (SimulatorFlashProgrammer) + Simulator directly:
// staging / erase / write / abort semantics, atomic commit, error codes, the
// alias mirror, 128 KiB images, and -- the central acceptance item -- the
// stale-TCG check: the SAME flash address must first execute build A's opcode
// and, after PROGRAM_END, build B's opcode (a surviving translation block
// would keep executing A). Finally A -> B -> A is cycled 100 times.
//
// The IPC path itself is covered by debug_ipc_selftest.
// Exit code 0 = PASS. Every phase prints PASS/FAIL.
// ============================================================================
#include <QCoreApplication>
#include <QElapsedTimer>

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "debug/SimulatorDebugTarget.h"
#include "debug/SimulatorFlashProgrammer.h"
#include "loader/HexLoader.h"
#include "sim/Simulator.h"

namespace {

int g_failures = 0;

void say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fputs(buf, stdout);
    std::fflush(stdout);
}

void check(bool ok, const char* what) {
    if (!ok) {
        say("  FAIL: %s\n", what);
        ++g_failures;
    }
}

struct PhaseGuard {
    const char* name;
    int before;
    explicit PhaseGuard(const char* n) : name(n), before(g_failures) {
        say("\n=== %s\n", name);
    }
    ~PhaseGuard() {
        say("=== %s: %s\n", name, g_failures == before ? "PASS" : "FAIL");
    }
};

// ---- firmware layout from the .debugmap (never hard-coded) ----------------
struct FwLayout {
    bool valid = false;
    uint32_t reset = 0;
    uint32_t code = 0;        // the differing instruction
    uint32_t magicAddr = 0;
    uint32_t resultAddr = 0;
    uint32_t magicValue = 0;  // A: 0xA1A1A1A1 / B: 0xB2B2B2B2
    uint32_t resultValue = 0; // A: 0x11 / B: 0x22
    uint32_t vecSp = 0, vecPc = 0;
    uint32_t loopAddr = 0;
    uint16_t opcode = 0;
};

FwLayout captureLayout(SimulatorDebugTarget& t) {
    FwLayout l;
    uint32_t map[16] = {};
    if (t.readMemory(0x08000200, map, sizeof(map)) != DebugStatus::Ok) return l;
    if (map[0] != 0x50524F47 || map[15] != 0x50524F47) return l;
    l.reset = map[1] & ~1u;
    l.code = map[2] & ~1u;
    l.magicAddr = map[3];
    l.resultAddr = map[4];
    l.magicValue = map[5];
    l.resultValue = map[6];
    l.loopAddr = map[8] & ~1u;
    uint32_t sp = 0, pc = 0;
    t.readMemory(0x08000000, &sp, 4);
    t.readMemory(0x08000004, &pc, 4);
    l.vecSp = sp;
    l.vecPc = pc & ~1u;
    t.readMemory(l.code, &l.opcode, 2);
    l.valid = true;
    return l;
}

// reset -> 6 exact single steps -> read what the firmware published in SRAM
bool runProgram(SimulatorDebugTarget& t, const FwLayout& l, uint32_t& magic,
                uint32_t& result) {
    if (t.resetHalt() != DebugStatus::Ok) return false;
    for (int i = 0; i < 6; i++) {
        if (t.step() != DebugStatus::Ok) return false;
    }
    return t.readMemory(l.magicAddr, &magic, 4) == DebugStatus::Ok &&
           t.readMemory(l.resultAddr, &result, 4) == DebugStatus::Ok;
}

// Full 128 KiB image from a hex file (0xFF outside the segments).
std::vector<uint8_t> imageFromHex(const QString& hexPath, bool& ok) {
    std::vector<uint8_t> image(128 * 1024, 0xFF);
    FirmwareImage img;
    std::string err;
    ok = false;
    if (!HexLoader::loadFile(hexPath.toStdString(), img, err)) {
        say("  HexLoader failed: %s\n", err.c_str());
        return image;
    }
    for (const auto& seg : img.segments) {
        if (seg.address < 0x08000000u ||
            uint64_t(seg.address) + seg.data.size() > 0x08020000u) {
            say("  hex segment outside flash\n");
            return image;
        }
        std::memcpy(image.data() + (seg.address - 0x08000000u), seg.data.data(),
                    seg.data.size());
    }
    ok = true;
    return image;
}

// Programs a whole image; returns the DebugStatus of programEnd.
DebugStatus programWholeImage(SimulatorFlashProgrammer& p,
                              const std::vector<uint8_t>& image,
                              uint64_t* tokenOut = nullptr) {
    uint64_t token = 0;
    DebugStatus s = p.programBegin(token);
    if (s != DebugStatus::Ok) return s;
    if (tokenOut) *tokenOut = token;
    s = p.programErase(token, 0x08000000u, uint32_t(image.size()));
    if (s != DebugStatus::Ok) return s;
    for (uint32_t off = 0; off < image.size(); off += 65536u) {
        const uint32_t len =
            std::min<uint32_t>(65536u, uint32_t(image.size() - off));
        s = p.programWrite(token, 0x08000000u + off, image.data() + off, len);
        if (s != DebugStatus::Ok) return s;
    }
    return p.programEnd(token);
}

}  // namespace

// ===========================================================================
int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    qRegisterMetaType<SimSnapshot>("SimSnapshot");

    const QString fwDir =
        QString::fromLocal8Bit(argc > 1 ? argv[1] : "firmware/");
    const QString hexA = fwDir + "test_program_a/test_program_a.hex";
    const QString hexB = fwDir + "test_program_b/test_program_b.hex";

    Simulator sim;
    SimulatorDebugTarget target(sim);
    SimulatorFlashProgrammer programmer(sim);
    say("debug_program_selftest: firmware dir %s\n",
        fwDir.toLocal8Bit().constData());

    FwLayout A, B;
    uint32_t magic = 0, result = 0;
    bool okA = false, okB = false;
    std::vector<uint8_t> imageA, imageB;

    {
        PhaseGuard ph("prepare: layouts A/B, images, A loaded");
        imageA = imageFromHex(hexA, okA);
        imageB = imageFromHex(hexB, okB);
        check(okA && okB, "HexLoader parsed both images");

        check(target.loadFirmware(hexA.toStdString()) == DebugStatus::Ok,
              "load test_program_a");
        A = captureLayout(target);
        check(A.valid, "A map readable");
        check(runProgram(target, A, magic, result), "run A");
        check(magic == A.magicValue && result == A.resultValue,
              "A publishes its expected values");

        check(target.loadFirmware(hexB.toStdString()) == DebugStatus::Ok,
              "load test_program_b");
        B = captureLayout(target);
        check(B.valid, "B map readable");
        check(runProgram(target, B, magic, result), "run B");
        check(magic == B.magicValue && result == B.resultValue,
              "B publishes its expected values");

        check(A.reset == B.reset && A.code == B.code && A.loopAddr == B.loopAddr,
              "A and B share the layout (same addresses)");
        check(A.opcode != B.opcode, "A/B opcodes differ at the same address");
        check(A.magicValue != B.magicValue &&
                  A.resultValue != B.resultValue,
              "A/B published values differ");
        say("  layout: reset=0x%08X code=0x%08X opA=0x%04X opB=0x%04X "
            "magicA=0x%08X magicB=0x%08X resultA=0x%X resultB=0x%X\n",
            A.reset, A.code, A.opcode, B.opcode, A.magicValue, B.magicValue,
            A.resultValue, B.resultValue);

        check(target.loadFirmware(hexA.toStdString()) == DebugStatus::Ok,
              "re-load A for the programming phases");
    }

    {
        PhaseGuard ph("A->B programming + stale TCG (hard requirement)");
        check(runProgram(target, A, magic, result) &&
                  magic == A.magicValue,
              "A executes before programming");
        const DebugStatus s = programWholeImage(programmer, imageB);
        check(s == DebugStatus::Ok, "program B image (begin/erase/write/end)");
        check(target.state() == TargetState::Halted,
              "PROGRAM_END leaves the target halted");
        check(target.stopInfo().reason == StopReason::Reset,
              "PROGRAM_END stop reason == Reset");
        uint64_t sp = 0, pc = 0;
        target.readRegister(DebugRegister::SP, sp);
        target.readRegister(DebugRegister::PC, pc);
        check(uint32_t(sp) == B.vecSp && (uint32_t(pc) & ~1u) == B.vecPc,
              "SP/PC come from B's vector table");
        uint16_t op = 0;
        target.readMemory(B.code, &op, 2);
        check(op == B.opcode, "flash now contains B's opcode at the same VA");
        uint32_t aliasSp = 0;
        target.readMemory(0x00000000u, &aliasSp, 4);
        check(aliasSp == B.vecSp, "0x00000000 alias mirrors the new flash");
        // THE stale-TCG check: A's opcode was executed/translated before; after
        // the commit the CPU must run B's opcode (result word = 0x22)
        check(runProgram(target, B, magic, result), "run B after programming");
        check(magic == B.magicValue && result == B.resultValue,
              "B's code (not the stale A translation) executed");
    }

    {
        PhaseGuard ph("B->A back-programming");
        check(programWholeImage(programmer, imageA) == DebugStatus::Ok,
              "program A back");
        check(runProgram(target, A, magic, result) &&
                  magic == A.magicValue && result == A.resultValue,
              "A executes again (opcode + magic)");
    }

    {
        PhaseGuard ph("abort never touches live flash");
        uint64_t token = 0;
        check(programmer.programBegin(token) == DebugStatus::Ok, "begin");
        check(programmer.programErase(token, 0x08000000u, 4096u) ==
                  DebugStatus::Ok,
              "erase (staging only)");
        check(programmer.programWrite(token, 0x08000000u, imageB.data(), 1024) ==
                  DebugStatus::Ok,
              "write half of B (staging only)");
        check(programmer.programActive(), "transaction is active");
        check(programmer.programAbort(token) == DebugStatus::Ok, "abort");
        check(!programmer.programActive(), "transaction closed by abort");
        check(runProgram(target, A, magic, result) && magic == A.magicValue,
              "abort did not pollute live flash (A still runs)");
        check(programmer.programAbort(token) == DebugStatus::ProgramNotActive,
              "repeated abort -> ProgramNotActive");
    }

    {
        PhaseGuard ph("token + transaction state errors");
        uint64_t token = 0;
        check(programmer.programErase(0x1234u, 0x08000000u, 16u) ==
                  DebugStatus::ProgramNotActive,
              "erase without a transaction -> ProgramNotActive");
        check(programmer.programWrite(0x1234u, 0x08000000u, imageA.data(), 16) ==
                  DebugStatus::ProgramNotActive,
              "write without a transaction -> ProgramNotActive");
        check(programmer.programEnd(0x1234u) == DebugStatus::ProgramNotActive,
              "end without a transaction -> ProgramNotActive");
        check(programmer.programBegin(token) == DebugStatus::Ok, "begin");
        check(programmer.programBegin(token) == DebugStatus::ProgramAlreadyActive,
              "second begin -> ProgramAlreadyActive");
        check(programmer.programErase(token + 1u, 0x08000000u, 16u) ==
                  DebugStatus::ProgramTokenInvalid,
              "wrong token -> ProgramTokenInvalid");
        check(programmer.programWrite(token + 1u, 0x08000000u, imageA.data(),
                                      16) == DebugStatus::ProgramTokenInvalid,
              "wrong token on write -> ProgramTokenInvalid");
        check(programmer.programEnd(token) == DebugStatus::Ok,
              "end with the right token");
        check(programmer.programEnd(token) == DebugStatus::ProgramNotActive,
              "end after commit -> ProgramNotActive");
        // running target: begin must refuse (never auto-halt)
        check(target.loadFirmware(hexA.toStdString()) == DebugStatus::Ok,
              "reload A");
        sim.startRun();
        check(sim.isRunning(), "target running");
        uint64_t token2 = 0;
        check(programmer.programBegin(token2) == DebugStatus::InvalidState,
              "programBegin on a running target -> InvalidState");
        check(programmer.programAbort(token2) == DebugStatus::ProgramNotActive,
              "abort while nothing is active");
        sim.pause();
    }

    {
        PhaseGuard ph("erase/write range validation");
        uint64_t token = 0;
        check(programmer.programBegin(token) == DebugStatus::Ok, "begin");
        check(programmer.programErase(token, 0x08000000u, 0u) == DebugStatus::Ok,
              "erase size 0 is an Ok no-op");
        check(programmer.programWrite(token, 0x08000000u, imageA.data(), 0) ==
                  DebugStatus::Ok,
              "write size 0 is an Ok no-op");
        check(programmer.programErase(token, 0x08020000u, 16u) ==
                  DebugStatus::ProgramRangeInvalid,
              "erase past the flash end -> ProgramRangeInvalid");
        check(programmer.programErase(token, 0x00000000u, 16u) ==
                  DebugStatus::ProgramRangeInvalid,
              "erase at the alias -> ProgramRangeInvalid (alias is read-only)");
        check(programmer.programErase(token, 0xFFFFFFF0u, 0x100u) ==
                  DebugStatus::ProgramRangeInvalid,
              "erase with an overflowing range -> ProgramRangeInvalid");
        check(programmer.programWrite(token, 0x08020000u, imageA.data(), 16) ==
                  DebugStatus::ProgramRangeInvalid,
              "write past the flash end -> ProgramRangeInvalid");
        check(programmer.programWrite(token, 0x20000000u, imageA.data(), 16) ==
                  DebugStatus::ProgramRangeInvalid,
              "write into RAM -> ProgramRangeInvalid");
        check(programmer.programWrite(token, 0xFFFFFFF0u, imageA.data(), 16) ==
                  DebugStatus::ProgramRangeInvalid,
              "write with an overflowing address -> ProgramRangeInvalid");
        check(programmer.programWrite(token, 0x08000000u, imageA.data(),
                                      128u * 1024u + 1u) ==
                  DebugStatus::ProgramRangeInvalid,
              "write beyond the transfer limit -> ProgramRangeInvalid");
        check(programmer.programAbort(token) == DebugStatus::Ok,
              "abort after the rejected operations");
        check(runProgram(target, A, magic, result) && result == A.resultValue,
              "live flash untouched by the rejected operations");
    }

    {
        PhaseGuard ph("overlap + out-of-order + full 128 KiB image");
        std::vector<uint8_t> x(0x80, 0xAA), y(0x80, 0x55);
        uint64_t token = 0;
        check(programmer.programBegin(token) == DebugStatus::Ok, "begin");
        check(programmer.programWrite(token, 0x08000100u, x.data(),
                                      uint32_t(x.size())) == DebugStatus::Ok,
              "write block X");
        check(programmer.programWrite(token, 0x08000140u, y.data(),
                                      uint32_t(y.size())) == DebugStatus::Ok,
              "write overlapping block Y (out of order)");
        // high block first, then low block
        check(programmer.programWrite(token, 0x08000080u, y.data(),
                                      uint32_t(y.size())) == DebugStatus::Ok,
              "write low block last");
        std::vector<uint8_t> staging;
        check(sim.readFlashImage(staging), "read the live image");
        // (the staging copy is what a commit would install; verified below)
        check(staging.size() == 128u * 1024u, "image size is 128 KiB");
        check(programmer.programEnd(token) == DebugStatus::Ok, "commit");
        std::vector<uint8_t> committed;
        check(sim.readFlashImage(committed), "read the committed image");
        auto allEqual = [&committed](uint32_t off, size_t n, uint8_t v) {
            if (off + n > committed.size()) return false;
            for (size_t i = 0; i < n; i++) {
                if (committed[off + i] != v) return false;
            }
            return true;
        };
        check(allEqual(0x140, 0x40, 0x55),
              "overlap: the last write wins in the staging image");
        check(allEqual(0x80, 0x40, 0x55),
              "out-of-order: the low block written last is what got committed");
        check(allEqual(0x100, 0x40, 0xAA),
              "X-only range keeps X (0xAA)");
        check(allEqual(0x140, 0x40, 0x55),
              "the overlapping range is covered by Y (last write wins)");
        bool headSame = true;
        for (size_t i = 0; i < 16; i++) {
            headSame = headSame && committed[i] == imageA[i];
        }
        check(headSame, "untouched bytes keep A's content");

        // full 128 KiB staged image
        std::vector<uint8_t> big(128 * 1024);
        for (size_t i = 0; i < big.size(); i++) {
            big[i] = uint8_t((i * 31u + 7u) & 0xFFu);
        }
        QElapsedTimer t;
        t.start();
        check(programWholeImage(programmer, big) == DebugStatus::Ok,
              "program a full 128 KiB pattern image");
        const qint64 ms = t.elapsed();
        say("  [perf] full 128 KiB (staging + commit + reset): %lld ms\n",
            (long long)ms);
        std::vector<uint8_t> readBack;
        check(sim.readFlashImage(readBack) && readBack == big,
              "128 KiB image committed byte for byte");
        check(ms < 20000, "128 KiB commit completes in a sane time");
    }

    {
        PhaseGuard ph("restore A and cycle A->B->A 100 times");
        check(programWholeImage(programmer, imageA) == DebugStatus::Ok,
              "restore A");
        check(runProgram(target, A, magic, result) && magic == A.magicValue,
              "A runs after restore");

        QElapsedTimer t;
        t.start();
        int bad = 0;
        for (int i = 0; i < 100; i++) {
            if (programWholeImage(programmer, imageB) != DebugStatus::Ok) {
                ++bad;
                break;
            }
            if (!runProgram(target, B, magic, result) ||
                magic != B.magicValue || result != B.resultValue) {
                ++bad;
                break;
            }
            if (programWholeImage(programmer, imageA) != DebugStatus::Ok) {
                ++bad;
                break;
            }
            if (!runProgram(target, A, magic, result) ||
                magic != A.magicValue || result != A.resultValue) {
                ++bad;
                break;
            }
        }
        const qint64 ms = t.elapsed();
        check(bad == 0, "100 x (B then A) cycles with correct execution");
        say("  [perf] 100 x A->B->A (200 commits + 200 runs): %lld ms\n",
            (long long)ms);
        std::vector<uint8_t> finalImage;
        check(sim.readFlashImage(finalImage) && finalImage == imageA,
              "live flash ends up as exactly A's image (no stale bytes)");
    }

    {
        PhaseGuard ph("Z post conditions");
        check(!programmer.programActive(), "no transaction left open");
        uint16_t op = 0;
        target.readMemory(A.code, &op, 2);
        check(op == A.opcode, "the final image contains A's opcode");
        if (!HeapValidate(GetProcessHeap(), 0, nullptr)) {
            say("  FAIL: heap corrupted by the programming paths\n");
            ++g_failures;
        }
    }

    say("\ntotal failures: %d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}