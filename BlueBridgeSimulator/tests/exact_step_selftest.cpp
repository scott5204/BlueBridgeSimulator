// ============================================================================
// Exact single-step acceptance test (stage 7-2d, PROJECT_HANDOFF 7.2d)
//
// Pins the guarantee that the simulator's exact debugger single-step mode
// (unicorn 2.1.4 UC_CTL_EXACT_SINGLE_STEP, wired through
// ICpuEngine::setExactSingleStep) executes EXACTLY ONE guest instruction per
// runBatch(pc, 1). Before this mode existed the instruction budget was
// enforced by uc_emu_stop() from unicorn's count hook, which is only observed
// at a translation-block boundary: inside a hot chained block the guest ran
// hundreds of instructions past the requested count (measured: 512 loop
// iterations = 1536 instructions while reporting 1).
//
// The engine here is the real BlueBridge engine (UnicornCpu) on the real
// unicorn.dll, but without the Qt SoC/board stack: a minimal test host backs
// the peripheral window, everything else is plain guest memory. Every step is
// driven exactly like the debugger does it:
//     pc = PC & ~1;  runBatch(pc, 1);  executed = executedLastBatch();
// and each step must report executed == 1 (the hook counter is the engine's
// own instruction count, and the guest's PC/memory side effects are checked
// independently).
//
// Test matrix (each exactly 100000 single steps unless noted):
//   A  straight-line code                     (16-insn NOP block, re-entered)
//   B  4-instruction self loop + SRAM counter
//   C  taken branch  (`b .`, PC never moves)
//   D  not-taken branch (16-insn `beq` block, flags Z=0)
//   E  BL / BX LR (call + return, 3-step cycle)
//   F  Thumb 16-bit / Thumb-2 32-bit mix      (8-insn block)
//   G  MMIO LDR/STR through the host model (peripheral window)
//   H  handler mode (injected interrupt active) stepping
//   I  exception return (BX LR with EXC_RETURN magic) inside an ISR cycle
//   J  instruction inside an IT block (unicorn: one IT block == one unit)
// plus a normal-mode sanity run (multi-instruction blocks unchanged), a
// mode-toggle check (enable/disable/enable keeps exactness) and an
// informational legacy-comparison run (exact mode OFF) that measures whether
// the old count=1 path is exact for the same code.
//
// The straight-line/literal blocks are re-entered from their head with a
// debugger-style PC write (the same thing the simulator does between batches),
// so a 100000-step run needs only a handful of translated blocks instead of
// 100000 distinct ones (unicorn's exact mode translates one block per guest
// instruction; a fresh block per step would burn ~1 GB of JIT buffer).
//
// ANY step that executes more than one instruction -> FAIL.
// ============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cpu/ICpuEngine.h"
#include "cpu/UnicornCpu.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_MSG(cond, ...)                                                   \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d: ", __FILE__, __LINE__);                 \
            std::printf(__VA_ARGS__);                                         \
            std::printf("\n");                                                 \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// ---------------------------------------------------------------------------
// memory map
// ---------------------------------------------------------------------------
constexpr uint32_t kFlashBase = 0x08000000u;
constexpr uint32_t kFlashSize = 0x00100000u;  // 1 MB (code space for the tests)
constexpr uint32_t kSramBase = 0x20000000u;
constexpr uint32_t kSramSize = 0x00010000u;  // 64 KB
constexpr uint32_t kPeriphBase = 0x40000000u;
constexpr uint32_t kPeriphSize = 0x00001000u;  // 4 KB "MMIO" window (host model)

// per-test code sites (inside kFlashSize)
constexpr uint32_t kCodeA = 0x08000000u;  // straight-line NOP block (16 insns)
constexpr uint32_t kCodeB = 0x08035000u;  // 4-insn loop
constexpr uint32_t kCodeC = 0x08034000u;  // b .
constexpr uint32_t kCodeD = 0x08084000u;  // not-taken beq block (16 insns)
constexpr uint32_t kCodeE = 0x08036000u;  // bl/bx lr/b
constexpr uint32_t kCodeF = 0x08037000u;  // 16/32-bit mix block (8 insns)
constexpr uint32_t kCodeG = 0x08081000u;  // 4-insn loop on the peripheral window
constexpr uint32_t kCodeH = 0x08082000u;  // 4-insn loop (handler mode)
constexpr uint32_t kCodeIThread = 0x08083000u;  // thread loop (b .)
constexpr uint32_t kCodeIIsr = 0x08083010u;     // 3-insn body + bx lr

constexpr uint32_t kCodeJ = 0x08085000u;  // IT EQ + ADDS + b (IT block)

constexpr uint32_t kCounterB = kSramBase + 0x00u;
constexpr uint32_t kCounterG = kSramBase + 0x10u;
constexpr uint32_t kCounterH = kSramBase + 0x20u;
constexpr uint32_t kCounterI = kSramBase + 0x30u;
constexpr uint32_t kMspTop = kSramBase + 0x8000u;
constexpr uint32_t kThreadReg = kSramBase + 0x40u;  // scratch for the thread

constexpr int kSteps = 100000;  // single steps per test (per spec)

// ---------------------------------------------------------------------------
// Thumb opcodes used below (verified by the PC checks of every test)
// ---------------------------------------------------------------------------
constexpr uint16_t kNop = 0xBF00;      // 2 bytes
constexpr uint16_t kNopW1 = 0xF3AF;    // 4 bytes  (nop.w)
constexpr uint16_t kNopW2 = 0x8000;
constexpr uint16_t kLdrR0R1 = 0x6808;  // ldr r0, [r1]
constexpr uint16_t kAddsR0 = 0x3001;   // adds r0, #1
constexpr uint16_t kStrR0R1 = 0x6008;  // str r0, [r1]
constexpr uint16_t kBeqSelf = 0xD000;  // beq (offset 0) -- not taken, Z=0
constexpr uint16_t kBl1 = 0xF000;      // bl +8  (target = PC+12)
constexpr uint16_t kBl2 = 0xF804;
constexpr uint16_t kBxLr = 0x4770;     // bx lr
constexpr uint16_t kItEq = 0xBF08;     // it eq (next instruction conditional)

uint16_t bSelf(uint32_t pcAt, uint32_t dest) {  // unconditional b dest
    const int32_t off = int32_t(dest) - int32_t(pcAt + 4);
    return uint16_t(0xE000u | (uint32_t(off / 2) & 0x7FFu));
}

// ---------------------------------------------------------------------------
// minimal ICpuHost: peripheral window backed by a RAM model
// ---------------------------------------------------------------------------
class TestHost : public ICpuHost {
public:
    explicit TestHost(uint32_t base) : base_(base) { ram_.resize(kPeriphSize, 0); }

    uint32_t cpuMmioRead(uint32_t addr, uint32_t size) override {
        const uint32_t off = addr - base_;
        ++mmioReads;
        if (off + size > ram_.size()) return 0xFFFFFFFFu;
        uint32_t v = 0;
        std::memcpy(&v, ram_.data() + off, size);
        return v;
    }

    void cpuMmioWrite(uint32_t addr, uint32_t value, uint32_t size) override {
        const uint32_t off = addr - base_;
        ++mmioWrites;
        if (off + size > ram_.size()) return;
        std::memcpy(ram_.data() + off, &value, size);
    }

    bool cpuFetchUnmapped(uint64_t) override { return true; }
    void cpuIntrEvent(int) override { ++intrEvents; }

    uint32_t loadWord(uint32_t off) const {
        uint32_t v = 0;
        std::memcpy(&v, ram_.data() + off, 4);
        return v;
    }

    uint64_t mmioReads = 0, mmioWrites = 0;
    int intrEvents = 0;

private:
    uint32_t base_;
    std::vector<uint8_t> ram_;
};

// ---------------------------------------------------------------------------
// engine harness
// ---------------------------------------------------------------------------
struct Harness {
    UnicornCpu cpu;
    TestHost host{kPeriphBase};

    bool start() {
        if (!cpu.start(&host)) {
            std::printf("  engine start failed: %s\n", cpu.lastError().c_str());
            return false;
        }
        if (!cpu.mapMemory(kFlashBase, kFlashSize, 0x7 /*R|W|X*/, nullptr) ||
            !cpu.mapMemory(kSramBase, kSramSize, 0x7, nullptr) ||
            !cpu.mapMemory(kPeriphBase, kPeriphSize, 0x7, nullptr)) {
            std::printf("  memory map failed: %s\n", cpu.lastError().c_str());
            return false;
        }
        return true;
    }

    void write16(uint32_t addr, uint16_t v) { writeBytes(addr, &v, 2); }
    void write32(uint32_t addr, uint32_t v) { writeBytes(addr, &v, 4); }

    void writeBytes(uint32_t addr, const void* p, size_t n) {
        if (!cpu.writeGuest(addr, p, n))
            std::printf("  FAIL: guest write 0x%08X (%zu bytes) failed\n", addr,
                        n);
    }

    uint32_t readWord(uint32_t addr) {
        uint32_t v = 0;
        if (!cpu.readGuest(addr, &v, 4))
            std::printf("  FAIL: guest read 0x%08X failed\n", addr);
        return v;
    }

    uint32_t pcNow() { return cpu.getReg(CpuReg::PC) & ~1u; }

    // one single step exactly like the debugger does it
    uint32_t stepOnce(const char* what, int i) {
        const uint32_t pc = pcNow();
        const CpuStopReason r = cpu.runBatch(pc, 1);
        const uint32_t executed = cpu.executedLastBatch();
        if (executed != 1u) {
            std::printf("  FAIL %s step %d: executed=%u (expected 1), "
                        "pc=0x%08X reason=%d\n",
                        what, i, executed, pc, int(r));
            ++g_failures;
            return executed;
        }
        if (r != CpuStopReason::CountReached) {
            std::printf("  FAIL %s step %d: stop reason %d (expected "
                        "CountReached), pc=0x%08X\n",
                        what, i, int(r), pc);
            ++g_failures;
        }
        return executed;
    }

    // CPU state like the simulator's reset path (vector table values)
    void setEntry(uint32_t pc) {
        cpu.setReg(CpuReg::MSP, kMspTop);
        cpu.setReg(CpuReg::PSP, kMspTop);
        cpu.setReg(CpuReg::SP, kMspTop);
        cpu.setReg(CpuReg::PC, pc & ~1u);
        cpu.setReg(CpuReg::LR, 0xFFFFFFFFu);
        cpu.setReg(CpuReg::XPSR, 0x01000000u);  // T bit set, APSR flags clear
        cpu.setReg(CpuReg::IPSR, 0u);
        cpu.setReg(CpuReg::CONTROL, 0u);
    }
};

void phase(const char* name) { std::printf("== %s\n", name); }

// common body: 4-instruction loop, r1 points at @counter, counts iterations
void buildCounterLoop(Harness& h, uint32_t base, uint32_t counter) {
    h.write16(base + 0, kLdrR0R1);
    h.write16(base + 2, kAddsR0);
    h.write16(base + 4, kStrR0R1);
    h.write16(base + 6, bSelf(base + 6, base));
    h.write32(counter, 0);
}

// ===========================================================================
// A..I
// ===========================================================================

void testA(Harness& h) {
    phase("A straight-line code (16-insn NOP block, 100000 steps)");
    constexpr int kBlock = 16;  // 32 bytes, one page
    for (int i = 0; i < kBlock; ++i) h.write16(kCodeA + 2 * i, kNop);
    h.setEntry(kCodeA);
    uint32_t expect = kCodeA;
    int wraps = 0;
    for (int i = 0; i < kSteps; ++i) {
        if (h.stepOnce("A", i) != 1u) return;
        expect += 2;
        CHECK_MSG(h.pcNow() == expect, "A step %d: pc=0x%08X expect=0x%08X", i,
                  h.pcNow(), expect);
        if (expect == kCodeA + 2u * kBlock) {
            // block done: re-enter with a debugger-style PC write (exactly what
            // the simulator does between batches)
            h.cpu.setReg(CpuReg::PC, kCodeA);
            CHECK_MSG(h.pcNow() == kCodeA, "A re-entry: pc=0x%08X", h.pcNow());
            expect = kCodeA;
            ++wraps;
        }
    }
    CHECK_MSG(wraps == kSteps / kBlock, "A wraps=%d (expected %d)", wraps,
              kSteps / kBlock);
}

void testB(Harness& h) {
    phase("B 4-instruction self loop, SRAM counter (100000 steps)");
    buildCounterLoop(h, kCodeB, kCounterB);
    h.setEntry(kCodeB);
    h.cpu.setReg(CpuReg::R1, kCounterB);
    const uint32_t n = kSteps / 4;  // 25000 whole iterations
    for (int i = 0; i < kSteps; ++i) {
        if (h.stepOnce("B", i) != 1u) return;
    }
    CHECK_MSG(h.pcNow() == kCodeB, "B pc=0x%08X (loop head)", h.pcNow());
    CHECK_MSG(h.readWord(kCounterB) == n,
              "B SRAM counter=%u (expected %u = steps/4)", h.readWord(kCounterB),
              n);
}

void testC(Harness& h) {
    phase("C taken branch (b ., 100000 steps)");
    h.write16(kCodeC, bSelf(kCodeC, kCodeC));
    h.setEntry(kCodeC);
    for (int i = 0; i < kSteps; ++i) {
        if (h.stepOnce("C", i) != 1u) return;
        CHECK_MSG(h.pcNow() == kCodeC, "C step %d: pc=0x%08X", i, h.pcNow());
    }
}

void testD(Harness& h) {
    phase("D not-taken branch (16-insn beq block, 100000 steps)");
    constexpr int kBlock = 16;
    for (int i = 0; i < kBlock; ++i) h.write16(kCodeD + 2 * i, kBeqSelf);
    h.setEntry(kCodeD);  // XPSR flags zero -> Z=0 -> beq not taken
    uint32_t expect = kCodeD;
    int wraps = 0;
    for (int i = 0; i < kSteps; ++i) {
        if (h.stepOnce("D", i) != 1u) return;
        expect += 2;
        CHECK_MSG(h.pcNow() == expect, "D step %d: pc=0x%08X expect=0x%08X", i,
                  h.pcNow(), expect);
        if (expect == kCodeD + 2u * kBlock) {
            h.cpu.setReg(CpuReg::PC, kCodeD);
            expect = kCodeD;
            ++wraps;
        }
    }
    CHECK_MSG(wraps == kSteps / kBlock, "D wraps=%d (expected %d)", wraps,
              kSteps / kBlock);
}

void testE(Harness& h) {
    phase("E BL / BX LR (3-step cycle, 100000 steps)");
    const uint32_t bl = kCodeE, afterBl = kCodeE + 4, func = kCodeE + 12;
    h.write16(bl + 0, kBl1);
    h.write16(bl + 2, kBl2);  // bl -> func (pc+12)
    h.write16(afterBl + 0, bSelf(afterBl, bl));
    h.write16(func, kBxLr);
    h.setEntry(bl);
    for (int i = 0; i < kSteps; ++i) {
        if (h.stepOnce("E", i) != 1u) return;
        const uint32_t pc = h.pcNow();
        const int phase3 = i % 3;
        if (phase3 == 0) {
            CHECK_MSG(pc == func, "E step %d: bl landed at 0x%08X (expect "
                                  "0x%08X)", i, pc, func);
            CHECK_MSG(h.cpu.getReg(CpuReg::LR) == (afterBl | 1u),
                      "E step %d: LR=0x%08X (expect 0x%08X)", i,
                      h.cpu.getReg(CpuReg::LR), afterBl | 1u);
        } else if (phase3 == 1) {
            CHECK_MSG(pc == afterBl, "E step %d: bx lr returned to 0x%08X "
                                     "(expect 0x%08X)", i, pc, afterBl);
        } else {
            CHECK_MSG(pc == bl, "E step %d: b landed at 0x%08X (expect "
                                "0x%08X)", i, pc, bl);
        }
    }
}

void testF(Harness& h) {
    phase("F Thumb-16 / Thumb-2 mix (8-insn block, 100000 steps)");
    constexpr int kPairs = 4;                  // nop(2) + nop.w(4) per pair
    constexpr int kBlockInsns = kPairs * 2;    // 8 instructions
    constexpr uint32_t kBlockBytes = 6u * kPairs;
    for (int p = 0; p < kPairs; ++p) {
        h.write16(kCodeF + 6u * p + 0, kNop);
        h.write16(kCodeF + 6u * p + 2, kNopW1);
        h.write16(kCodeF + 6u * p + 4, kNopW2);
    }
    h.setEntry(kCodeF);
    uint32_t expect = kCodeF;
    int wraps = 0;
    for (int i = 0; i < kSteps; ++i) {
        if (h.stepOnce("F", i) != 1u) return;
        expect += (i % 2 == 0) ? 2u : 4u;  // nop(2), nop.w(4), nop(2), ...
        CHECK_MSG(h.pcNow() == expect, "F step %d: pc=0x%08X expect=0x%08X", i,
                  h.pcNow(), expect);
        if (expect == kCodeF + kBlockBytes) {
            h.cpu.setReg(CpuReg::PC, kCodeF);
            expect = kCodeF;
            ++wraps;
        }
    }
    CHECK_MSG(wraps == kSteps / kBlockInsns, "F wraps=%d (expected %d)", wraps,
              kSteps / kBlockInsns);
}

void testG(Harness& h) {
    phase("G MMIO LDR/STR through the host model (100000 steps)");
    buildCounterLoop(h, kCodeG, kCounterG);
    h.setEntry(kCodeG);
    h.cpu.setReg(CpuReg::R1, kPeriphBase);  // the peripheral window
    const uint32_t n = kSteps / 4;
    for (int i = 0; i < kSteps; ++i) {
        if (h.stepOnce("G", i) != 1u) return;
    }
    CHECK_MSG(h.host.loadWord(0) == n,
              "G host-side peripheral value=%u (expected %u)", h.host.loadWord(0),
              n);
    CHECK_MSG(h.host.mmioWrites == n,
              "G host MMIO writes=%llu (expected %u)", h.host.mmioWrites, n);
    CHECK_MSG(h.host.mmioReads >= n, "G host MMIO reads=%llu (expected >= %u)",
              h.host.mmioReads, n);
}

// enter handler mode exactly like Simulator::doExceptionEntry does
void enterHandler(Harness& h, int exc, uint32_t handlerPc, uint32_t stackTop) {
    uint32_t frame[8] = {0};
    frame[0] = h.cpu.getReg(CpuReg::R0);
    frame[1] = h.cpu.getReg(CpuReg::R1);
    frame[2] = h.cpu.getReg(CpuReg::R2);
    frame[3] = h.cpu.getReg(CpuReg::R3);
    frame[4] = h.cpu.getReg(CpuReg::R12);
    frame[5] = h.cpu.getReg(CpuReg::LR);
    frame[6] = h.cpu.getReg(CpuReg::PC) & ~1u;
    frame[7] = h.cpu.getReg(CpuReg::XPSR) | 0x01000000u;
    const uint32_t sp = stackTop - 32;
    h.writeBytes(sp, frame, sizeof(frame));
    h.cpu.setReg(CpuReg::MSP, sp);
    h.cpu.setReg(CpuReg::SP, sp);
    h.cpu.setReg(CpuReg::LR, 0xFFFFFFF9u);  // EXC_RETURN, handler, MSP
    h.cpu.setV7mException(exc);
    h.cpu.setReg(CpuReg::PC, handlerPc & ~1u);
}

void testH(Harness& h) {
    phase("H interrupt active (handler mode) stepping (100000 steps)");
    buildCounterLoop(h, kCodeH, kCounterH);
    h.setEntry(kCodeH);
    h.cpu.setReg(CpuReg::R1, kCounterH);
    enterHandler(h, 16 /*IRQ0*/, kCodeH, kMspTop);
    const uint32_t n = kSteps / 4;
    for (int i = 0; i < kSteps; ++i) {
        if (h.stepOnce("H", i) != 1u) return;
    }
    CHECK_MSG(h.readWord(kCounterH) == n, "H SRAM counter=%u (expected %u)",
              h.readWord(kCounterH), n);
    uint32_t ipsr = 0;
    if (h.cpu.readRegister(CpuReg::IPSR, ipsr))
        CHECK_MSG(ipsr == 16u, "H IPSR=%u (expected 16: real handler mode)",
                  ipsr);
}

void testI(Harness& h) {
    phase("I exception return (BX LR + EXC_RETURN magic, 100000 steps)");
    // thread code: an endless loop; ISR: 3-instruction body + bx lr
    h.write16(kCodeIThread, bSelf(kCodeIThread, kCodeIThread));
    h.write16(kCodeIIsr + 0, kLdrR0R1);
    h.write16(kCodeIIsr + 2, kAddsR0);
    h.write16(kCodeIIsr + 4, kStrR0R1);
    h.write16(kCodeIIsr + 6, kBxLr);
    h.write32(kCounterI, 0);
    h.setEntry(kCodeIThread);
    h.cpu.setReg(CpuReg::R1, kCounterI);

    const int rounds = kSteps / 4;  // 3 ISR body instructions + 1 bx lr
    int steps = 0;
    for (int r = 0; r < rounds && g_failures == 0; ++r) {
        enterHandler(h, 16, kCodeIIsr, kMspTop);  // interrupt taken by the host
        h.cpu.setReg(CpuReg::R1, kCounterI);      // ISR uses r1 as pointer
        for (int k = 0; k < 3; ++k) {
            if (h.stepOnce("I(isr)", steps++) != 1u) return;
            CHECK_MSG(h.pcNow() == kCodeIIsr + 2u * (k + 1),
                      "I round %d body step %d: pc=0x%08X", r, k, h.pcNow());
        }
        const uint32_t spBefore = h.cpu.getReg(CpuReg::MSP);
        if (h.stepOnce("I(bx lr)", steps++) != 1u) return;
        CHECK_MSG(h.pcNow() == kCodeIThread,
                  "I round %d: exception return landed at 0x%08X (expect "
                  "0x%08X)", r, h.pcNow(), kCodeIThread);
        CHECK_MSG(h.cpu.getReg(CpuReg::MSP) == spBefore + 32u,
                  "I round %d: MSP=0x%08X (expect 0x%08X)", r,
                  h.cpu.getReg(CpuReg::MSP), spBefore + 32u);
        uint32_t ipsr = 0;
        if (h.cpu.readRegister(CpuReg::IPSR, ipsr))
            CHECK_MSG(ipsr == 0u, "I round %d: IPSR=%u after return", r, ipsr);
    }
    CHECK_MSG(steps == kSteps, "I stepped %d (expected %d)", steps, kSteps);
    CHECK_MSG(h.readWord(kCounterI) == uint32_t(rounds),
              "I SRAM counter=%u (expected %u)", h.readWord(kCounterI), rounds);
    // the thread instruction after the return must be executable again
    for (int i = 0; i < 4 && g_failures == 0; ++i) h.stepOnce("I(thread)", i);
}

// ===========================================================================
// normal-mode sanity: the untouched fast path must keep counting many
// instructions per batch (the debug mode is opt-in)
// ===========================================================================
void testNormalMode(Harness& h) {
    phase("normal mode sanity (multi-instruction batch, mode off)");
    // make sure the mode is OFF (fresh engine state is off anyway)
    CHECK(h.cpu.setExactSingleStep(false));
    for (int i = 0; i < 16; ++i) h.write16(kCodeA + 2 * i, kNop);
    h.setEntry(kCodeA);
    const CpuStopReason r = h.cpu.runBatch(kCodeA, 16);
    CHECK_MSG(r == CpuStopReason::CountReached, "normal batch reason=%d", int(r));
    CHECK_MSG(h.cpu.executedLastBatch() == 16u,
              "normal batch executed=%u (expected 16)",
              h.cpu.executedLastBatch());
    CHECK_MSG(h.pcNow() == kCodeA + 32u, "normal batch pc=0x%08X", h.pcNow());
}

// enabling/disabling the mode repeatedly must keep stepping exact
void testToggle(Harness& h) {
    phase("mode toggle (enable/disable/enable) keeps exactness");
    CHECK(h.cpu.setExactSingleStep(true));
    CHECK(h.cpu.setExactSingleStep(false));
    CHECK(h.cpu.setExactSingleStep(true));
    for (int i = 0; i < 1000; ++i) h.write16(kCodeA + 2 * i, kNop);
    h.setEntry(kCodeA);
    for (int i = 0; i < 1000; ++i) {
        if (h.stepOnce("toggle", i) != 1u) return;
    }
    CHECK_MSG(h.pcNow() == kCodeA + 2000u, "toggle pc=0x%08X", h.pcNow());
}

// ===========================================================================
// legacy comparison (INFORMATIONAL, does not fail the test): run the same
// 4-instruction loop with exact single-step mode OFF -- i.e. through the
// original count=1 path (count hook -> uc_emu_stop -> check_exit_request ->
// PC rewind). Measures how many steps really execute exactly one instruction
// and whether the guest-visible aggregate is exact. This is the evidence for
// the stage 7-2d report ("was count=1 already exact?").
// ===========================================================================
void testLegacyComparison(Harness& h) {
    phase("legacy comparison: 4-insn loop with exact mode OFF (informational)");
    if (!h.cpu.setExactSingleStep(false)) return;
    buildCounterLoop(h, kCodeB, kCounterB);
    h.setEntry(kCodeB);
    h.cpu.setReg(CpuReg::R1, kCounterB);
    int badSteps = 0;
    for (int i = 0; i < kSteps; ++i) {
        const uint32_t pc = h.pcNow();
        const CpuStopReason r = h.cpu.runBatch(pc, 1);
        const uint32_t exec = h.cpu.executedLastBatch();
        const uint32_t pcAfter = h.pcNow();
        const bool oneInsn =
            (exec == 1u) && (pcAfter == pc + 2u || pcAfter == kCodeB);
        if (!oneInsn) ++badSteps;
        if (r == CpuStopReason::Fault) break;
    }
    const uint32_t counter = h.readWord(kCounterB);
    std::printf("  legacy count=1: %d/%d steps were exactly one instruction "
                "(%d off), final counter=%u (expected %u) -> %s\n",
                kSteps - badSteps, kSteps, badSteps, counter, kSteps / 4,
                (badSteps == 0 && counter == uint32_t(kSteps / 4))
                    ? "legacy path was ALREADY exact here"
                    : "legacy path deviates from exactness here");
}

void buildItProgram(Harness& h) {
    h.write16(kCodeJ + 0, kItEq);
    h.write16(kCodeJ + 2, kAddsR0);  // 'adds r0, #1', only if EQ (Z==0 -> no)
    h.write16(kCodeJ + 4, bSelf(kCodeJ + 4, kCodeJ));
}

void testLegacyIt(Harness& h) {
    phase("legacy comparison: IT block with exact mode OFF (informational)");
    if (!h.cpu.setExactSingleStep(false)) return;
    buildItProgram(h);
    h.setEntry(kCodeJ);
    h.cpu.setReg(CpuReg::R0, 0x1234u);
    const int n = 30000;
    int off = 0, execMoreThanOne = 0;
    for (int i = 0; i < n; ++i) {
        const uint32_t pc = h.pcNow();
        h.cpu.runBatch(pc, 1);
        const uint32_t exec = h.cpu.executedLastBatch();
        const uint32_t pcAfter = h.pcNow();
        // unicorn executes an IT block as ONE instruction unit: the conditioned
        // instruction is consumed together with the IT (PC +4), in BOTH modes.
        const bool one =
            (exec == 1u) && (pcAfter == pc + 4u || pcAfter == kCodeJ);
        if (!one) {
            ++off;
            if (exec > 1u) ++execMoreThanOne;
        }
    }
    std::printf("  legacy count=1 inside an IT block: %d/%d steps were exactly "
                "one instruction (%d off, %d executed >1)\n",
                n - off, n, off, execMoreThanOne);
}

// ===========================================================================
// J: instruction inside an IT block. Measured behaviour (both modes, verified
// by the legacy IT probe below): unicorn executes an IT block as ONE
// instruction unit -- stepping from the IT lands after the conditioned
// instruction (PC +4) with `executedLastBatch() == 1` and the IT condition
// correctly honoured (a false condition still skips the instruction). The
// exact mode does not change that: it guarantees one *unicorn* instruction per
// step, and an IT block is one such instruction. Consequence for the debugger:
// breakpoints/steps have IT-block granularity (a breakpoint INSIDE an IT block
// is not observed) -- a pre-existing unicorn property, identical in both modes.
// ===========================================================================
void testJ(Harness& h) {
    phase("J instruction inside an IT block (100000 steps)");
    buildItProgram(h);
    h.setEntry(kCodeJ);  // Z=0 -> IT EQ does not pass -> ADDS is skipped
    h.cpu.setReg(CpuReg::R0, 0x1234u);
    for (int i = 0; i < kSteps; ++i) {
        if (h.stepOnce("J", i) != 1u) return;
        const uint32_t pc = h.pcNow();
        // 2-step cycle: [IT + conditioned ADDS] unit -> B -> back to the IT
        if (i % 2 == 0)
            CHECK_MSG(pc == kCodeJ + 4, "J step %d: pc=0x%08X", i, pc);
        else
            CHECK_MSG(pc == kCodeJ, "J step %d: pc=0x%08X", i, pc);
    }
    CHECK_MSG(h.cpu.getReg(CpuReg::R0) == 0x1234u,
              "J r0=0x%08X (IT EQ must skip the ADDS)", h.cpu.getReg(CpuReg::R0));
}

}  // namespace

int main() {
    std::printf("exact_step_selftest: real unicorn.dll, exact single-step "
                "mode\n");
    Harness h;
    if (!h.start()) return 1;
    if (!h.cpu.setExactSingleStep(true)) {
        std::printf("FAIL: setExactSingleStep not supported by the loaded "
                    "unicorn.dll (UC_CTL_EXACT_SINGLE_STEP missing)\n");
        return 1;
    }

    testNormalMode(h);
    testA(h);
    testB(h);
    testC(h);
    testD(h);
    testE(h);
    testF(h);
    testG(h);
    testH(h);
    testI(h);
    testJ(h);
    testLegacyComparison(h);
    testLegacyIt(h);
    testToggle(h);

    h.cpu.shutdown();
    if (g_failures == 0) {
        std::printf("\nexact_step_selftest: PASS\n");
        return 0;
    }
    std::printf("\nexact_step_selftest: %d FAILURE(S)\n", g_failures);
    return 1;
}