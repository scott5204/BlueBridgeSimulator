// ============================================================================
// Stage 7-1 acceptance test: Virtual Debug Target Core (IDebugTarget).
//
// Runs firmware/test_debug (a fixed-layout Thumb assembly program) against the
// real Simulator + SimulatorDebugTarget and checks every phase the stage spec
// lists (Test A .. Test U) plus a 10000x breakpoint hit/continue stress loop.
//
// test_debug layout (see firmware/test_debug/main.s, validated below):
//   0x08000040 Reset_Handler:  ldr r1,=0x20000100 ; nop
//   0x08000044 step_1:         movs r0,#1
//   0x08000046 step_2:         adds r0,r0,#1
//   0x08000048 step_3:         adds r0,r0,#1
//   0x0800004a step_4:         str r0,[r1]      -> SRAM[0x20000100] = r0
//   0x0800004c loop_top:       adds r2,r2,#1 ; str r2,[r1,#4] ; b loop_top
//   0x08000200 .debugmap:      magic + all label addresses + scratch addresses
//
// Exit code 0 = PASS. Every phase prints PASS/FAIL.
// ============================================================================
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "debug/SimulatorDebugTarget.h"
#include "sim/Simulator.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

#define CHECK_MSG(cond, ...)                                              \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d: %s | ", __FILE__, __LINE__, #cond); \
            std::printf(__VA_ARGS__);                                     \
            std::printf("\n");                                            \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

struct PhaseGuard {
    const char* name;
    int before;
    explicit PhaseGuard(const char* n) : name(n), before(g_failures) {
        std::printf("\n=== %s\n", name);
    }
    ~PhaseGuard() {
        std::printf("=== %s: %s\n", name,
                    g_failures == before ? "PASS" : "FAIL");
    }
};

const char* st(DebugStatus s) { return debug::toString(s); }

// ---- test_debug debug map (firmware/test_debug/main.s + linker.ld) ---------
constexpr uint32_t kMapBase = 0x08000200u;
constexpr uint32_t kMapMagic = 0xDEB06001u;
constexpr uint32_t kMapEndMagic = 0xDEB06E0Du;
enum MapWord {
    MAP_MAGIC = 0,
    MAP_RESET = 1,
    MAP_STEP1 = 2,
    MAP_STEP2 = 3,
    MAP_STEP3 = 4,
    MAP_STEP4 = 5,
    MAP_LOOP = 6,
    MAP_SCRATCH = 7,
    MAP_COUNTER = 8,
    MAP_SP = 9,
    MAP_END = 15,
};

struct Map {
    uint32_t w[16] = {};
    bool load(SimulatorDebugTarget& t) {
        return t.readMemory(kMapBase, w, sizeof(w)) == DebugStatus::Ok;
    }
    uint32_t raw(int i) const { return w[i]; }
    uint32_t operator[](int i) const { return w[i] & ~1u; }  // Thumb-normalized
};

// ---- helpers ---------------------------------------------------------------
uint32_t readWord(SimulatorDebugTarget& t, uint32_t addr) {
    uint32_t v = 0;
    const DebugStatus s = t.readMemory(addr, &v, 4);
    if (s != DebugStatus::Ok) {
        std::printf("  FAIL readMemory(0x%08X) = %s\n", addr, st(s));
        g_failures++;
    }
    return v;
}

uint32_t reg(SimulatorDebugTarget& t, DebugRegister r) {
    uint64_t v = 0;
    const DebugStatus s = t.readRegister(r, v);
    if (s != DebugStatus::Ok) {
        std::printf("  FAIL readRegister = %s\n", st(s));
        g_failures++;
    }
    return uint32_t(v);
}

uint32_t pc(SimulatorDebugTarget& t) { return reg(t, DebugRegister::PC) & ~1u; }

// Drive the REAL 1:1 run loop (no event loop in a headless test, so runTick is
// invoked directly -- exactly what reload_selftest does) until the target
// stops, or the wall-clock guard expires.
bool runUntilStopped(Simulator& sim, int maxMs) {
    QElapsedTimer w;
    w.start();
    while (sim.isRunning() && w.elapsed() < maxMs) {
        QMetaObject::invokeMethod(&sim, "runTick", Qt::DirectConnection);
    }
    return !sim.isRunning();
}

// Resume and let it run for a fixed wall time (then halt).
void runFor(Simulator& sim, int ms) {
    QElapsedTimer w;
    w.start();
    while (sim.isRunning() && w.elapsed() < ms) {
        QMetaObject::invokeMethod(&sim, "runTick", Qt::DirectConnection);
    }
    sim.pause();
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    qRegisterMetaType<SimSnapshot>("SimSnapshot");

    const QString dir = QString::fromLocal8Bit(argc > 1 ? argv[1] : "firmware/");
    const QString debugHex = dir + "test_debug/test_debug.hex";
    const QString fullHex = dir + "test_full/test_full.hex";

    Simulator sim;
    SimulatorDebugTarget tgt(sim);
    SimSnapshot snap;
    int snapCount = 0;
    QObject::connect(&sim, &Simulator::stateChanged,
                     [&](const SimSnapshot& s) { snap = s; ++snapCount; });

    std::printf("debug_target_selftest: firmware %s\n",
                debugHex.toLocal8Bit().constData());

    // ---- load + validate the debug map ------------------------------------
    {
        PhaseGuard ph("load + debug map");
        CHECK(tgt.loadFirmware(debugHex.toStdString()) == DebugStatus::Ok);
        CHECK(tgt.state() == TargetState::Halted);
        Map map;
        CHECK(map.load(tgt));
        std::printf("  map: magic=0x%08X reset=0x%08X step1..4=0x%08X/0x%08X/"
                    "0x%08X/0x%08X loop=0x%08X scratch=0x%08X counter=0x%08X "
                    "sp=0x%08X\n",
                    map.raw(MAP_MAGIC), map[MAP_RESET], map[MAP_STEP1],
                    map[MAP_STEP2], map[MAP_STEP3], map[MAP_STEP4],
                    map[MAP_LOOP], map.raw(MAP_SCRATCH), map.raw(MAP_COUNTER),
                    map.raw(MAP_SP));
        // map validation: never trust hard-coded addresses
        CHECK(map.raw(MAP_MAGIC) == kMapMagic);
        CHECK(map.raw(MAP_END) == kMapEndMagic);
        const uint32_t vecSp = readWord(tgt, 0x08000000);
        const uint32_t vecReset = readWord(tgt, 0x08000004) & ~1u;
        CHECK(map[MAP_RESET] == vecReset);
        CHECK(map[MAP_SP] == vecSp);
        CHECK(map[MAP_STEP1] > map[MAP_RESET] &&
              map[MAP_STEP2] > map[MAP_STEP1] &&
              map[MAP_STEP3] > map[MAP_STEP2] &&
              map[MAP_STEP4] > map[MAP_STEP3] &&
              map[MAP_LOOP] > map[MAP_STEP4]);
        CHECK(map[MAP_LOOP] < kMapBase);  // code stays below the debug map
        CHECK(map.raw(MAP_SCRATCH) == 0x20000100u);
        CHECK(map.raw(MAP_COUNTER) == 0x20000104u);
    }

    Map map;
    CHECK(map.load(tgt));

    // ======================================================================
    // Test A: reset halt
    // ======================================================================
    {
        PhaseGuard ph("A reset halt");
        CHECK(tgt.resetHalt() == DebugStatus::Ok);
        CHECK_MSG(tgt.state() == TargetState::Halted, "state=%s",
                  debug::toString(tgt.state()));
        CHECK(tgt.stopInfo().reason == StopReason::Reset);
        CHECK(tgt.stopInfo().pc == map[MAP_RESET]);
        CHECK(reg(tgt, DebugRegister::SP) == map.raw(MAP_SP));
        CHECK(pc(tgt) == map[MAP_RESET]);
        // virtual time must not advance while halted
        const uint64_t c0 = snap.cycles;
        const int n0 = snapCount;
        QThread::msleep(30);
        CHECK(snap.cycles == c0);
        CHECK(snapCount == n0);
        std::printf("  halted: cycles frozen at %llu over 30 ms wall time\n",
                    (unsigned long long)c0);
    }

    // ======================================================================
    // Test B: register read
    // ======================================================================
    {
        PhaseGuard ph("B register read (R0-R12/SP/LR/PC/XPSR)");
        const DebugRegister required[] = {
            DebugRegister::R0, DebugRegister::R1, DebugRegister::R2,
            DebugRegister::R3, DebugRegister::R4, DebugRegister::R5,
            DebugRegister::R6, DebugRegister::R7, DebugRegister::R8,
            DebugRegister::R9, DebugRegister::R10, DebugRegister::R11,
            DebugRegister::R12, DebugRegister::SP, DebugRegister::LR,
            DebugRegister::PC, DebugRegister::XPSR};
        for (DebugRegister r : required) {
            uint64_t v = 0;
            const DebugStatus s = tgt.readRegister(r, v);
            CHECK_MSG(s == DebugStatus::Ok, "reg %d -> %s", int(r), st(s));
        }
        const uint32_t p = pc(tgt);
        CHECK(p >= 0x08000000u && p < 0x08020000u);
        std::printf("  PC=0x%08X SP=0x%08X XPSR=0x%08X\n", p,
                    reg(tgt, DebugRegister::SP), reg(tgt, DebugRegister::XPSR));
        // optional (MSP/PSP/masks/CONTROL + FPU): report, do not fail
        const DebugRegister optional[] = {
            DebugRegister::MSP,       DebugRegister::PSP,
            DebugRegister::PRIMASK,   DebugRegister::BASEPRI,
            DebugRegister::FAULTMASK, DebugRegister::CONTROL,
            DebugRegister::S0,        DebugRegister::S31,
            DebugRegister::FPSCR};
        for (DebugRegister r : optional) {
            uint64_t v = 0;
            const DebugStatus s = tgt.readRegister(r, v);
            std::printf("  optional reg %2d: %-16s value=0x%08X\n", int(r), st(s),
                        uint32_t(v));
            CHECK(s == DebugStatus::Ok || s == DebugStatus::Unsupported);
        }
    }

    // ======================================================================
    // Test C: register write
    // ======================================================================
    {
        PhaseGuard ph("C register write");
        const struct {
            DebugRegister r;
            uint32_t v;
        } cases[] = {{DebugRegister::R0, 0x12345678u},
                     {DebugRegister::R7, 0xA5A5A5A5u},
                     {DebugRegister::R12, 0x0BADF00Du}};
        for (auto& c : cases) {
            CHECK(tgt.writeRegister(c.r, c.v) == DebugStatus::Ok);
            CHECK_MSG(reg(tgt, c.r) == c.v, "write 0x%08X read 0x%08X", c.v,
                      reg(tgt, c.r));
        }
        // PC write: write a known valid Thumb instruction address, read back,
        // then restore -- never step while PC points at the test address
        CHECK(tgt.writeRegister(DebugRegister::PC, map[MAP_STEP2]) ==
              DebugStatus::Ok);
        CHECK((pc(tgt)) == map[MAP_STEP2]);
        CHECK(tgt.writeRegister(DebugRegister::PC, map[MAP_RESET]) ==
              DebugStatus::Ok);
        CHECK(pc(tgt) == map[MAP_RESET]);
        std::printf("  R0/R7/R12 + PC round-trip ok (PC restored to 0x%08X)\n",
                    pc(tgt));
    }

    // ======================================================================
    // Test D: SRAM memory (1/2/4/16 bytes + unaligned)
    // ======================================================================
    {
        PhaseGuard ph("D SRAM memory access");
        const uint32_t base = map.raw(MAP_SCRATCH);
        // 1 byte
        uint8_t b8 = 0xA5;
        CHECK(tgt.writeMemory(base + 0x20, &b8, 1) == DebugStatus::Ok);
        uint8_t rb8 = 0;
        CHECK(tgt.readMemory(base + 0x20, &rb8, 1) == DebugStatus::Ok);
        CHECK(rb8 == b8);
        // 2 bytes
        uint16_t h16 = 0xBEEF;
        CHECK(tgt.writeMemory(base + 0x22, &h16, 2) == DebugStatus::Ok);
        uint16_t rh16 = 0;
        CHECK(tgt.readMemory(base + 0x22, &rh16, 2) == DebugStatus::Ok);
        CHECK(rh16 == h16);
        // 4 bytes (little endian: EF BE AD DE in memory order)
        uint32_t w32 = 0xDEADBEEFu;
        CHECK(tgt.writeMemory(base + 0x24, &w32, 4) == DebugStatus::Ok);
        uint32_t rw32 = 0;
        CHECK(tgt.readMemory(base + 0x24, &rw32, 4) == DebugStatus::Ok);
        CHECK(rw32 == w32);
        uint8_t bytes[4] = {};
        CHECK(tgt.readMemory(base + 0x24, bytes, 4) == DebugStatus::Ok);
        CHECK(bytes[0] == 0xEF && bytes[1] == 0xBE && bytes[2] == 0xAD &&
              bytes[3] == 0xDE);
        // 16 bytes
        uint8_t buf[16];
        for (int i = 0; i < 16; i++) buf[i] = uint8_t(0x10 + i);
        CHECK(tgt.writeMemory(base + 0x28, buf, 16) == DebugStatus::Ok);
        uint8_t rbuf[16] = {};
        CHECK(tgt.readMemory(base + 0x28, rbuf, 16) == DebugStatus::Ok);
        CHECK(std::memcmp(buf, rbuf, 16) == 0);
        // unaligned
        uint8_t ub[7];
        for (int i = 0; i < 7; i++) ub[i] = uint8_t(0xD0 + i);
        CHECK(tgt.writeMemory(base + 0x33, ub, 7) == DebugStatus::Ok);
        uint8_t rub[7] = {};
        CHECK(tgt.readMemory(base + 0x33, rub, 7) == DebugStatus::Ok);
        CHECK(std::memcmp(ub, rub, 7) == 0);
        std::printf("  SRAM 1/2/4/16/7-unaligned byte-exact round-trips ok\n");
    }

    // ======================================================================
    // Test E: CCM memory
    // ======================================================================
    {
        PhaseGuard ph("E CCM memory access");
        const uint32_t ccmBase = 0x10000000u + 0x100u;
        uint32_t v = 0x5A5A1234u;
        const DebugStatus w = tgt.writeMemory(ccmBase, &v, 4);
        uint32_t r = 0;
        const DebugStatus rd = tgt.readMemory(ccmBase, &r, 4);
        CHECK_MSG(w == DebugStatus::Ok, "write -> %s", st(w));
        CHECK_MSG(rd == DebugStatus::Ok, "read -> %s", st(rd));
        CHECK(r == v);
        std::printf("  CCM 0x%08X: wrote 0x%08X read 0x%08X\n", ccmBase, v, r);
    }

    // ======================================================================
    // Test F: invalid addresses never crash
    // ======================================================================
    {
        PhaseGuard ph("F invalid address handling");
        uint8_t tmp[8] = {};
        const uint32_t bad[] = {0x90000000u, 0x30000000u, 0x60000000u};
        for (uint32_t a : bad) {
            CHECK_MSG(tgt.readMemory(a, tmp, 4) == DebugStatus::InvalidAddress,
                      "read 0x%08X", a);
            CHECK_MSG(tgt.writeMemory(a, tmp, 4) == DebugStatus::InvalidAddress,
                      "write 0x%08X", a);
        }
        // range straddling the end of SRAM
        CHECK(tgt.readMemory(0x20000000u + 32u * 1024u - 2u, tmp, 4) ==
              DebugStatus::InvalidAddress);
        // flash write is Unsupported (no flash programming in this stage)
        CHECK(tgt.writeMemory(0x08000000u, tmp, 4) == DebugStatus::Unsupported);
        CHECK(tgt.readMemory(0, nullptr, 4) == DebugStatus::InvalidAddress);
        CHECK(tgt.readMemory(0x20000000u, tmp, 0) == DebugStatus::InvalidAddress);
        std::printf("  out-of-map read/write -> InvalidAddress, flash write -> "
                    "Unsupported\n");
    }

    // ======================================================================
    // Test G: flash read (incl. the 0x00000000 boot alias)
    // ======================================================================
    {
        PhaseGuard ph("G flash read + boot alias");
        uint32_t vec[2] = {};
        CHECK(tgt.readMemory(0x08000000u, vec, 8) == DebugStatus::Ok);
        std::printf("  vector: SP=0x%08X Reset=0x%08X\n", vec[0], vec[1]);
        CHECK(vec[0] == map.raw(MAP_SP));
        CHECK((vec[1] & ~1u) == map[MAP_RESET]);
        uint32_t alias[2] = {};
        CHECK(tgt.readMemory(0x00000000u, alias, 8) == DebugStatus::Ok);
        CHECK(alias[0] == vec[0] && alias[1] == vec[1]);
        uint32_t magic = 0;
        CHECK(tgt.readMemory(kMapBase, &magic, 4) == DebugStatus::Ok);
        CHECK(magic == kMapMagic);
    }

    // ======================================================================
    // Test H: single step #1 (real execution of "movs r0,#1")
    // ======================================================================
    {
        PhaseGuard ph("H single step #1");
        tgt.clearBreakpoints();
        CHECK(tgt.resetHalt() == DebugStatus::Ok);
        // A Cortex-M reset does not architecturally define R0-R12 and this
        // simulator (like the hardware) does not clear them -- Test C wrote
        // R0, so zero it explicitly to make the "R0 == 1 after movs" claim
        // unambiguous.
        CHECK(tgt.writeRegister(DebugRegister::R0, 0) == DebugStatus::Ok);
        // run to step_1 (Reset_Handler is "ldr r1,=..." + "nop")
        int guard = 0;
        while (pc(tgt) != map[MAP_STEP1] && guard++ < 16) {
            CHECK(tgt.step() == DebugStatus::Ok);
        }
        CHECK(pc(tgt) == map[MAP_STEP1]);
        CHECK(reg(tgt, DebugRegister::R0) == 0u);
        const uint64_t cycles0 = snap.cycles;
        CHECK(tgt.step() == DebugStatus::Ok);
        CHECK(tgt.state() == TargetState::Halted);
        CHECK(tgt.stopInfo().reason == StopReason::SingleStep);
        CHECK(tgt.stopInfo().pc == map[MAP_STEP2]);
        CHECK(pc(tgt) == map[MAP_STEP2]);
        CHECK_MSG(reg(tgt, DebugRegister::R0) == 1u, "R0=0x%08X",
                  reg(tgt, DebugRegister::R0));
        // exactly one instruction of virtual time
        CHECK(snap.cycles == cycles0 + 1u);
        std::printf("  step 0x%08X -> 0x%08X, R0=1, cycles %llu -> %llu (+1)\n",
                    map[MAP_STEP1], pc(tgt),
                    (unsigned long long)cycles0,
                    (unsigned long long)snap.cycles);
    }

    // ======================================================================
    // Test I: single step #2/#3
    // ======================================================================
    {
        PhaseGuard ph("I single step #2");
        CHECK(tgt.step() == DebugStatus::Ok);
        CHECK_MSG(reg(tgt, DebugRegister::R0) == 2u, "R0=0x%08X",
                  reg(tgt, DebugRegister::R0));
        CHECK(pc(tgt) == map[MAP_STEP3]);
        CHECK(tgt.stopInfo().reason == StopReason::SingleStep);
        CHECK(tgt.step() == DebugStatus::Ok);
        CHECK(reg(tgt, DebugRegister::R0) == 3u);
        CHECK(pc(tgt) == map[MAP_STEP4]);
        std::printf("  R0 = 2 then 3, PC now 0x%08X\n", pc(tgt));
    }

    // ======================================================================
    // Test J: single step store -- proves real execution (not a PC tweak)
    // ======================================================================
    {
        PhaseGuard ph("J single step store");
        CHECK(pc(tgt) == map[MAP_STEP4]);
        CHECK(readWord(tgt, map.raw(MAP_SCRATCH)) == 0u);  // STR not yet run
        CHECK(tgt.step() == DebugStatus::Ok);              // "str r0,[r1]"
        CHECK(pc(tgt) == map[MAP_LOOP]);
        const uint32_t stored = readWord(tgt, map.raw(MAP_SCRATCH));
        CHECK_MSG(stored == 3u, "SRAM[0x%08X]=0x%08X",
                  map.raw(MAP_SCRATCH), stored);
        std::printf("  STR executed for real: SRAM[0x%08X]=%u (R0)\n",
                    map.raw(MAP_SCRATCH), stored);
    }

    // ======================================================================
    // Test K: breakpoint basic (stop BEFORE the target instruction)
    // ======================================================================
    {
        PhaseGuard ph("K breakpoint basic");
        tgt.clearBreakpoints();
        CHECK(tgt.resetHalt() == DebugStatus::Ok);
        CHECK(tgt.addBreakpoint(map[MAP_STEP3]) == DebugStatus::Ok);
        CHECK(tgt.resume() == DebugStatus::Ok);
        CHECK(runUntilStopped(sim, 500));
        CHECK(tgt.state() == TargetState::Halted);
        CHECK_MSG(tgt.stopInfo().reason == StopReason::Breakpoint, "reason=%s",
                  debug::toString(tgt.stopInfo().reason));
        CHECK(tgt.stopInfo().pc == map[MAP_STEP3]);
        CHECK(pc(tgt) == map[MAP_STEP3]);
        CHECK_MSG(reg(tgt, DebugRegister::R0) == 2u,
                  "R0=0x%08X (step_3 ran?)", reg(tgt, DebugRegister::R0));
        // 4 instructions really executed before the hit: ldr, nop, step_1, step_2
        CHECK_MSG(snap.cycles == 4u, "cycles=%llu",
                  (unsigned long long)snap.cycles);
        std::printf("  hit at 0x%08X with R0=2 (instruction NOT executed), "
                    "cycles=4\n",
                    pc(tgt));
    }

    // ======================================================================
    // Test L: continue from the breakpoint (no immediate re-hit)
    // ======================================================================
    {
        PhaseGuard ph("L continue from breakpoint");
        CHECK(pc(tgt) == map[MAP_STEP3]);  // still sitting on the breakpoint
        CHECK(tgt.resume() == DebugStatus::Ok);
        // one tick: the target must still be RUNNING (not instantly re-stopped)
        QMetaObject::invokeMethod(&sim, "runTick", Qt::DirectConnection);
        CHECK_MSG(sim.isRunning(), "state=%s", debug::toString(tgt.state()));
        runFor(sim, 60);
        CHECK(tgt.state() == TargetState::Halted);
        CHECK(tgt.stopInfo().reason == StopReason::UserHalt);
        CHECK_MSG(reg(tgt, DebugRegister::R0) == 3u,
                  "R0=0x%08X (step_3 must have executed)", 
                  reg(tgt, DebugRegister::R0));
        CHECK(readWord(tgt, map.raw(MAP_SCRATCH)) == 3u);  // step_4 ran too
        CHECK(pc(tgt) != map[MAP_STEP3]);                  // moved on
        CHECK(readWord(tgt, map.raw(MAP_COUNTER)) > 0u);   // loop is running
        std::printf("  passed 0x%08X, R0=3, loop counter=%u, PC=0x%08X\n",
                    map[MAP_STEP3], readWord(tgt, map.raw(MAP_COUNTER)), pc(tgt));
    }

    // ======================================================================
    // Test M: step from a breakpoint (executes that one instruction)
    // ======================================================================
    {
        PhaseGuard ph("M step from breakpoint");
        tgt.clearBreakpoints();
        CHECK(tgt.resetHalt() == DebugStatus::Ok);
        CHECK(tgt.addBreakpoint(map[MAP_STEP3]) == DebugStatus::Ok);
        CHECK(tgt.resume() == DebugStatus::Ok);
        CHECK(runUntilStopped(sim, 500));
        CHECK(tgt.stopInfo().reason == StopReason::Breakpoint);
        CHECK(pc(tgt) == map[MAP_STEP3]);
        CHECK(readWord(tgt, map.raw(MAP_SCRATCH)) == 0u);
        // step: the instruction AT the breakpoint runs exactly once
        CHECK(tgt.step() == DebugStatus::Ok);
        CHECK(tgt.stopInfo().reason == StopReason::SingleStep);
        CHECK_MSG(pc(tgt) == map[MAP_STEP4], "PC=0x%08X", pc(tgt));
        CHECK(reg(tgt, DebugRegister::R0) == 3u);
        CHECK(readWord(tgt, map.raw(MAP_SCRATCH)) == 0u);  // step_4 not yet
        std::printf("  step over breakpoint: 0x%08X -> 0x%08X, R0=3\n",
                    map[MAP_STEP3], pc(tgt));
    }

    // ======================================================================
    // Test N: remove breakpoint -> execution passes it without stopping
    // ======================================================================
    {
        PhaseGuard ph("N remove breakpoint");
        tgt.clearBreakpoints();
        CHECK(tgt.resetHalt() == DebugStatus::Ok);
        CHECK(tgt.addBreakpoint(map[MAP_STEP3]) == DebugStatus::Ok);
        CHECK(tgt.removeBreakpoint(map[MAP_STEP3]) == DebugStatus::Ok);
        CHECK(!sim.debugHasBreakpoint(map[MAP_STEP3]));
        CHECK(tgt.resume() == DebugStatus::Ok);
        runFor(sim, 60);
        CHECK(tgt.stopInfo().reason == StopReason::UserHalt);  // no BP hit
        CHECK(reg(tgt, DebugRegister::R0) == 3u);
        CHECK(readWord(tgt, map.raw(MAP_COUNTER)) > 0u);
        std::printf("  passed the removed breakpoint, loop counter=%u\n",
                    readWord(tgt, map.raw(MAP_COUNTER)));
    }

    // ======================================================================
    // Test O: multiple breakpoints (4) + removal of one
    // ======================================================================
    {
        PhaseGuard ph("O multiple breakpoints");
        tgt.clearBreakpoints();
        CHECK(tgt.resetHalt() == DebugStatus::Ok);
        const uint32_t bps[4] = {map[MAP_STEP1], map[MAP_STEP2], map[MAP_STEP3],
                                 map[MAP_STEP4]};
        for (uint32_t a : bps) CHECK(tgt.addBreakpoint(a) == DebugStatus::Ok);
        for (int i = 0; i < 4; i++) {
            CHECK(tgt.resume() == DebugStatus::Ok);
            CHECK(runUntilStopped(sim, 500));
            CHECK_MSG(tgt.stopInfo().reason == StopReason::Breakpoint,
                      "hit %d reason=%s", i,
                      debug::toString(tgt.stopInfo().reason));
            CHECK_MSG(pc(tgt) == bps[i], "hit %d: PC=0x%08X expected 0x%08X", i,
                      pc(tgt), bps[i]);
        }
        std::printf("  4 breakpoints hit in order 0x%08X/0x%08X/0x%08X/0x%08X\n",
                    bps[0], bps[1], bps[2], bps[3]);
        // remove step_2: step_1 and step_3 must still hit, step_2 must pass
        CHECK(tgt.removeBreakpoint(map[MAP_STEP2]) == DebugStatus::Ok);
        CHECK(tgt.resetHalt() == DebugStatus::Ok);
        CHECK(tgt.resume() == DebugStatus::Ok);
        CHECK(runUntilStopped(sim, 500));
        CHECK(pc(tgt) == map[MAP_STEP1]);
        CHECK(tgt.resume() == DebugStatus::Ok);
        CHECK(runUntilStopped(sim, 500));
        CHECK_MSG(pc(tgt) == map[MAP_STEP3], "PC=0x%08X (removed BP still hit?)",
                  pc(tgt));
        std::printf("  after removing 0x%08X: still hits 0x%08X -> 0x%08X\n",
                    map[MAP_STEP2], map[MAP_STEP1], pc(tgt));
    }

    // ======================================================================
    // Test P: user halt freezes the whole virtual board
    // ======================================================================
    {
        PhaseGuard ph("P user halt (virtual time frozen)");
        tgt.clearBreakpoints();
        CHECK(tgt.resetHalt() == DebugStatus::Ok);
        CHECK(tgt.resume() == DebugStatus::Ok);
        runFor(sim, 60);  // resume already paused at the end of runFor
        CHECK(tgt.state() == TargetState::Halted);
        CHECK(tgt.stopInfo().reason == StopReason::UserHalt);
        const uint64_t cycles0 = snap.cycles;
        const uint32_t counter0 = readWord(tgt, map.raw(MAP_COUNTER));
        const int snaps0 = snapCount;
        QThread::msleep(40);  // do not run the simulator
        CHECK(snap.cycles == cycles0);
        CHECK(snapCount == snaps0);
        CHECK_MSG(readWord(tgt, map.raw(MAP_COUNTER)) == counter0,
                  "firmware advanced while halted!");
        // halt() while halted is a safe no-op and must not change the reason
        CHECK(tgt.halt() == DebugStatus::Ok);
        CHECK(tgt.stopInfo().reason == StopReason::UserHalt);
        std::printf("  40 ms wall time halted: cycles %llu, loop counter %u, no "
                    "state changes\n",
                    (unsigned long long)cycles0, counter0);
    }

    // ======================================================================
    // Test Q: resume does NOT replay the halted wall time (1:1 baseline)
    //
    // The meaningful direction is the UPPER bound: virtual time is measured
    // from the start of THIS run, so 40 ms spent halted would show up as a
    // jump far beyond the wall clock. (The lower bound cannot demand a full
    // 1.0x: a tight 3-instruction loop crosses the always-on UC_HOOK_CODE for
    // every instruction, which caps this host at ~16-20 MIPS = ~0.2x of
    // 80 MHz. That is a pre-existing property of the normal run path, not
    // something the debug core introduced.)
    // ======================================================================
    {
        PhaseGuard ph("Q resume realtime baseline");
        const uint64_t cyclesBefore = snap.cycles;
        QElapsedTimer w;
        w.start();
        CHECK(tgt.resume() == DebugStatus::Ok);
        // track the largest single snapshot-to-snapshot jump as well
        uint64_t prev = snap.cycles;
        uint64_t maxJump = 0;
        while (sim.isRunning() && w.elapsed() < 120) {
            QMetaObject::invokeMethod(&sim, "runTick", Qt::DirectConnection);
            if (snap.cycles != prev) {
                maxJump = std::max(maxJump, snap.cycles - prev);
                prev = snap.cycles;
            }
        }
        sim.pause();
        const qint64 wallMs = w.elapsed();
        const double advMs = double(snap.cycles - cyclesBefore) / 80000.0;
        const double maxJumpMs = double(maxJump) / 80000.0;
        std::printf("  halted 40 ms, then %lld ms wall -> %.1f ms virtual "
                    "(largest jump %.1f ms)\n",
                    (long long)wallMs, advMs, maxJumpMs);
        // no catch-up: the 40 ms spent halted must NOT be replayed
        CHECK_MSG(advMs <= double(wallMs) + 15.0,
                  "virtual %.1f ms in %lld ms wall (catch-up!)", advMs,
                  (long long)wallMs);
        CHECK_MSG(maxJumpMs <= 45.0, "single jump %.1f ms", maxJumpMs);
        // and the clock really advances after the halt
        CHECK_MSG(advMs >= 5.0, "virtual %.1f ms in %lld ms wall (stalled?)",
                  advMs, (long long)wallMs);
    }

    // ======================================================================
    // Test R: reset halt after running
    // ======================================================================
    {
        PhaseGuard ph("R reset halt after running");
        CHECK(tgt.resume() == DebugStatus::Ok);
        runFor(sim, 40);
        CHECK(pc(tgt) != map[MAP_RESET]);  // PC left the reset handler
        CHECK(tgt.resetHalt() == DebugStatus::Ok);
        CHECK(tgt.state() == TargetState::Halted);
        CHECK(tgt.stopInfo().reason == StopReason::Reset);
        CHECK(reg(tgt, DebugRegister::SP) == map.raw(MAP_SP));
        CHECK(pc(tgt) == map[MAP_RESET]);
        CHECK(snap.cycles == 0u);  // SoC reset zeroes the virtual clock
        std::printf("  SP=0x%08X PC=0x%08X cycles=0\n",
                    reg(tgt, DebugRegister::SP), pc(tgt));
    }

    // ======================================================================
    // Test S: reset run
    // ======================================================================
    {
        PhaseGuard ph("S reset run");
        CHECK(tgt.resetRun() == DebugStatus::Ok);
        CHECK(tgt.state() == TargetState::Running);
        runFor(sim, 60);
        CHECK(tgt.state() == TargetState::Halted);
        CHECK(tgt.stopInfo().reason == StopReason::UserHalt);
        CHECK(readWord(tgt, map.raw(MAP_COUNTER)) > 0u);
        std::printf("  running after reset: loop counter=%u\n",
                    readWord(tgt, map.raw(MAP_COUNTER)));
    }

    // ======================================================================
    // Test T: firmware reload regression (test_debug -> test_full)
    // ======================================================================
    {
        PhaseGuard ph("T firmware reload regression");
        tgt.clearBreakpoints();
        CHECK(tgt.loadFirmware(debugHex.toStdString()) == DebugStatus::Ok);
        CHECK(tgt.state() == TargetState::Halted);
        // Zig-zag: load test_full WHILE RUNNING -> must auto-pause, not refuse
        CHECK(tgt.resume() == DebugStatus::Ok);
        CHECK(tgt.loadFirmware(fullHex.toStdString()) == DebugStatus::Ok);
        CHECK_MSG(!sim.isRunning(), "load while running did not auto-pause");
        CHECK(tgt.state() == TargetState::Halted);
        CHECK(tgt.resetHalt() == DebugStatus::Ok);
        CHECK(tgt.resume() == DebugStatus::Ok);
        runFor(sim, 400);
        const uint32_t magic = readWord(tgt, 0x20000000u);
        std::printf("  after reload: test_full mailbox magic=0x%08X, LED=0x%02X\n",
                    magic, snap.leds[0] ? 1 : 0);
        CHECK_MSG(magic == 0xC0FFEE05u, "magic=0x%08X", magic);
        // and the reload target must really execute: LED chaser (single bit)
        bool anyLed = false;
        for (int i = 0; i < 8; i++) anyLed = anyLed || snap.leds[i] != 0;
        CHECK(anyLed);
        // a bad path must be rejected, not silently "succeed"
        CHECK(tgt.loadFirmware((dir + "no_such_firmware.hex").toStdString()) ==
              DebugStatus::CpuError);
    }

    // ======================================================================
    // Test U: existing MMIO / peripherals still work after the debug core
    // ======================================================================
    {
        PhaseGuard ph("U existing MMIO still works (test_full)");
        CHECK(tgt.resetHalt() == DebugStatus::Ok);
        CHECK(tgt.resume() == DebugStatus::Ok);
        runFor(sim, 400);
        const uint32_t led = readWord(tgt, 0x20000000u + 4u * 3u);  // LED pattern
        const uint32_t page = readWord(tgt, 0x20000000u + 4u * 2u);
        std::printf("  test_full: LED pattern=0x%02X page=%u pa7=%.1f Hz duty "
                    "%.0f%% systick rvr=%u\n",
                    led, page, snap.pa7Frequency, snap.pa7Duty * 100.0,
                    snap.systickRvr);
        CHECK(led != 0u && (led & (led - 1u)) == 0u);  // chaser: single bit
        CHECK(snap.systickRvr != 0u);                  // SysTick programmed
        CHECK(snap.pa7Active);                         // TIM3_CH2 PWM on PA7
        CHECK_MSG(snap.pa7Frequency > 900.0 && snap.pa7Frequency < 1100.0,
                  "PA7 = %.1f Hz", snap.pa7Frequency);
        CHECK(snap.pa7Duty > 0.4 && snap.pa7Duty < 0.6);
    }

    // ======================================================================
    // Breakpoint stress: 100000 hit/continue cycles.
    //
    // Stage spec section 30: plan A (UC_HOOK_CODE + uc_emu_stop) had to be
    // validated with >= 10000 cycles before it may be used. It FAILED that
    // validation on this unicorn build -- inside a hot chained TB the stop
    // overshot by 512 loop iterations (the target instruction executed
    // hundreds of times) -- so the simulator uses plan B: host-side precise
    // breakpoints, one guest instruction per batch (see
    // Simulator::runBreakpointBatch), now backed by unicorn's exact
    // single-step mode (UC_CTL_EXACT_SINGLE_STEP, stage 7-2d) so a count=1
    // batch is guaranteed to execute exactly one instruction.
    // This loop pins that semantics: every cycle must stop exactly ON the
    // breakpoint address with the loop counter having advanced by exactly ONE
    // (which is exactly 3 guest instructions for this loop body), for >=
    // 100000 cycles in a row.
    // ======================================================================
    {
        PhaseGuard ph("stress 100000x breakpoint hit/continue");
        tgt.clearBreakpoints();
        CHECK(tgt.loadFirmware(debugHex.toStdString()) == DebugStatus::Ok);
        CHECK(tgt.resetHalt() == DebugStatus::Ok);
        // Deterministic loop state: the debug firmware keeps its loop count in
        // r2 AND mirrors it into SRAM. Reset loads SP/PC from the vector table
        // but does NOT clear the general-purpose registers, so r2 is a leftover
        // from the previous (wall-clock paced) phases: with r2 = 512 the very
        // first continue would mirror 513 into the counter -- that looked like
        // a stepped-overshoot but was a test-state artifact. Own both values.
        CHECK(tgt.writeRegister(DebugRegister::R2, 0) == DebugStatus::Ok);
        const uint32_t zero = 0;
        CHECK(tgt.writeMemory(map.raw(MAP_COUNTER), &zero, 4) ==
              DebugStatus::Ok);
        CHECK(tgt.addBreakpoint(map[MAP_LOOP]) == DebugStatus::Ok);
        // first hit: reached through the real resume path
        CHECK(tgt.resume() == DebugStatus::Ok);
        CHECK(runUntilStopped(sim, 500));
        CHECK(tgt.stopInfo().reason == StopReason::Breakpoint);
        CHECK(pc(tgt) == map[MAP_LOOP]);
        // 100000 continues. Each cycle: verify PC/PC-reason/instruction-not-
        // executed, then continue via the same skip-once primitive the debugger
        // resume uses, one batch through the normal pipeline (no wall pacing,
        // so the stress loop stays cheap while still hitting the real hook).
        const int kIters = 100000;
        const int failBase = g_failures;  // earlier phases may have failed
        QElapsedTimer stress;
        stress.start();
        int iter = 0;
        for (; iter < kIters && g_failures == failBase; ++iter) {
            if (tgt.stopInfo().reason != StopReason::Breakpoint ||
                pc(tgt) != map[MAP_LOOP]) {
                std::printf("  FAIL iteration %d: reason=%s PC=0x%08X\n", iter,
                            debug::toString(tgt.stopInfo().reason), pc(tgt));
                g_failures++;
                break;
            }
            const uint32_t counter = readWord(tgt, map.raw(MAP_COUNTER));
            if (counter != uint32_t(iter)) {
                std::printf("  FAIL iteration %d: loop counter=%u\n", iter,
                            counter);
                g_failures++;
                break;
            }
            sim.debugSkipCurrentBreakpointOnce();
            const uint32_t counterBefore = readWord(tgt, map.raw(MAP_COUNTER));
            const uint64_t cyclesBefore = snap.cycles;
            sim.slotStep(1);
            const uint32_t counterAfter = readWord(tgt, map.raw(MAP_COUNTER));
            // HARD requirement (stage 7-2d): the continue must stop exactly on
            // the breakpoint after ONE loop iteration -- 3 guest instructions,
            // no overshoot. counter +1 / cycles +3 is that exactness check.
            if (counterAfter != counterBefore + 1u ||
                snap.cycles != cyclesBefore + 3u) {
                std::printf("  FAIL iteration %d: breakpoint overshoot "
                            "(counter %u->%u, cycles +%lld, expected +1/+3; "
                            "bps=%zu running=%d pc=0x%08X loop=0x%08X)\n",
                            iter, counterBefore, counterAfter,
                            (long long)(snap.cycles - cyclesBefore),
                            sim.breakpoints().size(), sim.isRunning() ? 1 : 0,
                            pc(tgt), map[MAP_LOOP]);
                g_failures++;
                break;
            }
        }
        const uint32_t finalCounter = readWord(tgt, map.raw(MAP_COUNTER));
        const qint64 stressMs = stress.elapsed();
        std::printf("  %d hit/continue cycles, loop counter=%u, PC=0x%08X, "
                    "reason=%s\n",
                    iter, finalCounter, pc(tgt),
                    debug::toString(tgt.stopInfo().reason));
        std::printf("  precise-breakpoint mode: %lld ms for %d continue(s) "
                    "(%.1fus per instruction, %d instructions each)\n",
                    (long long)stressMs, iter,
                    stressMs * 1000.0 / double(iter * 3), 3);
        CHECK(iter == kIters);
        CHECK(finalCounter == uint32_t(kIters));
        CHECK(tgt.stopInfo().reason == StopReason::Breakpoint);
        CHECK(pc(tgt) == map[MAP_LOOP]);
        CHECK(reg(tgt, DebugRegister::R2) == uint32_t(kIters));  // r2 == loops
    }

    // ---------------------------------------------------------------------
    if (g_failures == 0) {
        std::printf("\ndebug_target_selftest: PASS\n");
        return 0;
    }
    std::printf("\ndebug_target_selftest: %d FAILURE(S)\n", g_failures);
    return 1;
}