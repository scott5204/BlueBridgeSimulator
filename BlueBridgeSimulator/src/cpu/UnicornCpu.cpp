#include "cpu/UnicornCpu.h"

#include <cstdio>

#include "unicorn/arm.h"

UnicornCpu::~UnicornCpu() { shutdown(); }

int UnicornCpu::mapReg(CpuReg reg) {
    switch (reg) {
    case CpuReg::R0: return UC_ARM_REG_R0;
    case CpuReg::R1: return UC_ARM_REG_R1;
    case CpuReg::R2: return UC_ARM_REG_R2;
    case CpuReg::R3: return UC_ARM_REG_R3;
    case CpuReg::R4: return UC_ARM_REG_R4;
    case CpuReg::R5: return UC_ARM_REG_R5;
    case CpuReg::R6: return UC_ARM_REG_R6;
    case CpuReg::R7: return UC_ARM_REG_R7;
    case CpuReg::R8: return UC_ARM_REG_R8;
    case CpuReg::R9: return UC_ARM_REG_R9;
    case CpuReg::R10: return UC_ARM_REG_R10;
    case CpuReg::R11: return UC_ARM_REG_R11;
    case CpuReg::R12: return UC_ARM_REG_R12;
    case CpuReg::SP: return UC_ARM_REG_SP;
    case CpuReg::LR: return UC_ARM_REG_LR;
    case CpuReg::PC: return UC_ARM_REG_PC;
    case CpuReg::XPSR: return UC_ARM_REG_XPSR;
    case CpuReg::IPSR: return UC_ARM_REG_IPSR;
    case CpuReg::APSR: return UC_ARM_REG_APSR;
    case CpuReg::MSP: return UC_ARM_REG_MSP;
    case CpuReg::PSP: return UC_ARM_REG_PSP;
    case CpuReg::PRIMASK: return UC_ARM_REG_PRIMASK;
    case CpuReg::BASEPRI: return UC_ARM_REG_BASEPRI;
    case CpuReg::FAULTMASK: return UC_ARM_REG_FAULTMASK;
    case CpuReg::CONTROL: return UC_ARM_REG_CONTROL;
    case CpuReg::FPSCR: return UC_ARM_REG_FPSCR;
    default:
        // S0..S31 are contiguous in the unicorn enum (verified against
        // third_party/unicorn/include/unicorn/arm.h: S0=152 ... S31=183).
        if (reg >= CpuReg::S0 && reg <= CpuReg::S31) {
            return UC_ARM_REG_S0 + (int(reg) - int(CpuReg::S0));
        }
        return UC_ARM_REG_INVALID;
    }
}

bool UnicornCpu::start(ICpuHost* host) {
    api_ = &unicornApi();
    if (!api_->ok()) {
        lastError_ = api_->loadError();
        return false;
    }
    host_ = host;

    unsigned int major = 0, minor = 0;
    api_->uc_version(&major, &minor);
    (void)major;
    (void)minor;

    uc_engine* uc = nullptr;
    uc_err err = api_->uc_open(UC_ARCH_ARM,
                               uc_mode(UC_MODE_THUMB | UC_MODE_MCLASS), &uc);
    if (err != UC_ERR_OK) {
        lastError_ = std::string("uc_open failed: ") + api_->uc_strerror(err);
        return false;
    }
    uc_ = uc;

    // Select the Cortex-M4 model (ARMv7-M + DSP + VFPv4). Fall back through
    // close relatives if this unicorn build lacks a specific model.
    const int models[] = {UC_CPU_ARM_CORTEX_M4, UC_CPU_ARM_CORTEX_M7,
                          UC_CPU_ARM_CORTEX_M33, UC_CPU_ARM_CORTEX_M3};
    bool modelOk = false;
    for (int m : models) {
        if (api_->uc_ctl(uc, UC_CTL_WRITE(UC_CTL_CPU_MODEL, 1), m) ==
            UC_ERR_OK) {
            modelOk = true;
            break;
        }
    }
    if (!modelOk) {
        lastError_ = "uc_ctl(UC_CTL_CPU_MODEL) failed for all Cortex-M models";
        return false;
    }

    // Memory hooks over the peripheral space.
    api_->uc_hook_add(uc, &hRead_, UC_HOOK_MEM_READ,
                      reinterpret_cast<void*>(&UnicornCpu::hookMemRead), this,
                      kMmioLo, kMmioHi);
    api_->uc_hook_add(uc, &hWrite_, UC_HOOK_MEM_WRITE,
                      reinterpret_cast<void*>(&UnicornCpu::hookMemWrite), this,
                      kMmioLo, kMmioHi);
    api_->uc_hook_add(uc, &hFetch_, UC_HOOK_MEM_FETCH_UNMAPPED,
                      reinterpret_cast<void*>(&UnicornCpu::hookFetchUnmapped),
                      this, 1, 0);
    api_->uc_hook_add(uc, &hIntr_, UC_HOOK_INTR,
                      reinterpret_cast<void*>(&UnicornCpu::hookIntr), this, 1,
                      0);
    // Instruction counter / breakpoint hook (hook everything).
    api_->uc_hook_add(uc, &hCode_, UC_HOOK_CODE,
                      reinterpret_cast<void*>(&UnicornCpu::hookCode), this, 1,
                      0);
    return true;
}

void UnicornCpu::shutdown() {
    if (uc_) {
        api_->uc_close(uc_);
        uc_ = nullptr;
    }
}

bool UnicornCpu::mapMemory(uint64_t base, size_t size, uint32_t perms,
                           void* backing) {
    if (!uc_) return false;
    uc_err err;
    if (backing) {
        err = api_->uc_mem_map_ptr(uc_, base, size, perms, backing);
    } else {
        err = api_->uc_mem_map(uc_, base, size, perms);
    }
    if (err != UC_ERR_OK) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "map 0x%08llX (0x%zX) failed: %s",
                      static_cast<unsigned long long>(base), size,
                      api_->uc_strerror(err));
        lastError_ = buf;
        return false;
    }
    return true;
}

uint32_t UnicornCpu::getReg(CpuReg reg) {
    uint32_t v = 0;
    if (uc_) api_->uc_reg_read(uc_, mapReg(reg), &v);
    return v;
}

void UnicornCpu::setReg(CpuReg reg, uint32_t value) {
    if (uc_) api_->uc_reg_write(uc_, mapReg(reg), &value);
}

// Checked access for the debugger path: reports whether the engine really
// implements the register (some are accepted but silently ignored by the
// M-profile core, e.g. writes to read-only status registers).
bool UnicornCpu::readRegister(CpuReg reg, uint32_t& value) {
    if (!uc_) return false;
    const int id = mapReg(reg);
    if (id == UC_ARM_REG_INVALID) return false;
    return api_->uc_reg_read(uc_, id, &value) == UC_ERR_OK;
}

bool UnicornCpu::writeRegister(CpuReg reg, uint32_t value) {
    if (!uc_) return false;
    const int id = mapReg(reg);
    if (id == UC_ARM_REG_INVALID) return false;
    return api_->uc_reg_write(uc_, id, &value) == UC_ERR_OK;
}

bool UnicornCpu::setCpacr(uint32_t value) {
    if (!uc_) return false;
    if (cpacrMode_ == 0 || cpacrMode_ == 1) {
        if (api_->uc_reg_write(uc_, UC_ARM_REG_C1_C0_2, &value) ==
            UC_ERR_OK) {
            cpacrMode_ = 1;
            return true;
        }
    }
    if (cpacrMode_ == 0 || cpacrMode_ == 2) {
        uc_arm_cp_reg cp{};
        cp.cp = 15;
        cp.is64 = 0;
        cp.sec = 0;
        cp.crn = 1;
        cp.crm = 0;
        cp.opc1 = 0;
        cp.opc2 = 2;
        cp.val = value;
        if (api_->uc_reg_write(uc_, UC_ARM_REG_CP_REG, &cp) == UC_ERR_OK) {
            cpacrMode_ = 2;
            return true;
        }
    }
    return false;
}

bool UnicornCpu::writeGuest(uint64_t addr, const void* data, size_t size) {
    return uc_ && api_->uc_mem_write(uc_, addr, data, size) == UC_ERR_OK;
}

bool UnicornCpu::readGuest(uint64_t addr, void* data, size_t size) {
    return uc_ && api_->uc_mem_read(uc_, addr, data, size) == UC_ERR_OK;
}

bool UnicornCpu::invalidateRegion(uint64_t base, uint64_t end) {
    if (!uc_) return false;
    // drop cached translation blocks covering [base, end)
    return api_->uc_ctl(uc_, UC_CTL_WRITE(UC_CTL_TB_REMOVE_CACHE, 2), base,
                        end) == UC_ERR_OK;
}

void UnicornCpu::requestStop() {
    stopRequested_.store(true);
    if (uc_) api_->uc_emu_stop(uc_);
}

CpuStopReason UnicornCpu::runBatch(uint32_t pc, uint32_t insnCount) {
    if (!uc_) return CpuStopReason::Fault;
    excReturnPending_ = false;
    excReturnValue_ = 0;
    excReturnHandled_ = false;
    intrStop_ = false;
    batchExecuted_ = 0;

    // If the batch stops early (exception return / stop request),
    // executedLastBatch_ reflects the true instruction count collected by the
    // UC_HOOK_CODE counter.
    //
    // Stage 7-2 finding (PROJECT_HANDOFF 7.2d): the old instruction-count limit
    // (uc_emu_stop from unicorn's count hook, only observed at a block
    // boundary) let a chained TB keep running past the requested count
    // (measured: one count=1 batch executed 512 loop iterations = 1536
    // instructions while reporting 1). FIXED in unicorn-2.1.4 with
    // UC_CTL_EXACT_SINGLE_STEP: in exact mode every translated block holds
    // exactly one guest instruction and never chains, and the budget is
    // enforced exactly at the block boundary. The debugger paths
    // (debugStepOne / runBreakpointBatch) enable it via setExactSingleStep();
    // the normal run path keeps multi-instruction blocks and is unchanged.
    uc_err err = api_->uc_emu_start(uc_, static_cast<uint64_t>(pc) | 1ull, 0,
                                    0, insnCount);
    executedLastBatch_ = batchExecuted_;

    // Invariant guard for exact single-step mode: a count=1 batch must execute
    // exactly one instruction. If this ever fires, the engine ran more than
    // requested (a stale translation survived the mode switch, or the mode flag
    // was lost) -- print both the host and the engine view instead of silently
    // overshooting the debugger's step.
    if (exactStep_ && insnCount == 1 && batchExecuted_ != 1) {
        int engineFlag = -1;
        const uc_err q = api_->uc_ctl(
            uc_, UC_CTL_READ(UC_CTL_EXACT_SINGLE_STEP, 1), &engineFlag);
        std::fprintf(stderr,
                     "[bb] exact-step violated: pc=0x%08X executed=%u "
                     "hostFlag=1 engineFlag=%d (query=%d) err=%d\n",
                     pc, batchExecuted_, engineFlag, int(q), int(err));
        std::fflush(stderr);
    }

    // A real exception frame was popped in a hook (the ISR returned). The
    // engine has already restored r0-r3/r12/lr/xpsr/sp/pc and left Handler
    // mode, so the batch is a normal continuation of the main loop -- not a
    // fault, even though the underlying emu_start may have reported an
    // unmapped-fetch stop on the magic PC.
    if (excReturnHandled_) return CpuStopReason::CountReached;
    if (stopRequested_.load()) return CpuStopReason::StopRequested;
    if (err != UC_ERR_OK) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "CPU fault @pc=0x%08X: %s (uc_err=%d)",
                      getReg(CpuReg::PC), api_->uc_strerror(err), int(err));
        lastError_ = buf;
        return CpuStopReason::Fault;
    }
    return CpuStopReason::CountReached;
}

void UnicornCpu::setV7mException(int exc) {
    if (uc_ && api_->setV7mException)
        api_->setV7mException(uc_, exc);
}

// Exact single-step mode: every translated block is limited to one guest
// instruction and never chains, so a count=1 batch ends after exactly one
// instruction (see unicorn-2.1.4: UC_CTL_EXACT_SINGLE_STEP). Toggling flushes
// unicorn's translation cache, so the state is cached here and the control is
// only written when it actually changes.
bool UnicornCpu::setExactSingleStep(bool enable) {
    if (!uc_) return false;
    if (exactStep_ == enable) return true;
    const uc_err err = api_->uc_ctl(
        uc_, UC_CTL_WRITE(UC_CTL_EXACT_SINGLE_STEP, 1), enable ? 1 : 0);
    if (err != UC_ERR_OK) return false;
    exactStep_ = enable;
    return true;
}

// Real Cortex-M exception return. Reads the exception-return magic that the
// core loaded into PC (BX LR / POP {PC} from a Handler) and pops the actual
// 8-word stack frame the simulator pushed on exception entry, restoring the
// interrupted Thread-mode registers. Because this runs inside the engine
// hooks it is a genuine in-core register/memory operation.
void UnicornCpu::popExceptionFrame() {
    const uint32_t magic = getReg(CpuReg::PC) | 1u;  // ensure bit0 (Thumb)
    const bool usePsp = (magic & 4u) != 0;            // SPSEL bit (bit2)
    const CpuReg spReg = usePsp ? CpuReg::PSP : CpuReg::MSP;

    uint32_t sp = getReg(spReg);
    uint32_t frame[8];
    if (!readGuest(sp, frame, sizeof(frame))) {
        return;  // frame not readable -> leave core untouched
    }

    setReg(CpuReg::R0, frame[0]);
    setReg(CpuReg::R1, frame[1]);
    setReg(CpuReg::R2, frame[2]);
    setReg(CpuReg::R3, frame[3]);
    setReg(CpuReg::R12, frame[4]);
    setReg(CpuReg::LR, frame[5]);
    setReg(CpuReg::XPSR, frame[7] | 0x01000000u);  // restore APSR + keep T bit
    setReg(spReg, sp + 32);
    setReg(CpuReg::PC, frame[6] & ~1u);

    // Leave Handler mode so the resumed Thread code runs normally.
    setV7mException(0);
    excReturnHandled_ = true;
}

// ---------------------------------------------------------------------------
// Hook callbacks (engine thread)
// ---------------------------------------------------------------------------
void UnicornCpu::hookMemRead(uc_engine* uc, uc_mem_type type,
                             uint64_t address, int size, int64_t value,
                             void* user) {
    (void)uc;
    (void)type;
    (void)value;
    UnicornCpu* self = static_cast<UnicornCpu*>(user);
    // Compute the value through the SoC model, then plant it in guest memory
    // so the pending load observes it (hook fires BEFORE the read).
    uint32_t v = self->host_->cpuMmioRead(static_cast<uint32_t>(address),
                                          uint32_t(size));
    self->api_->uc_mem_write(uc, address, &v, size_t(size));
}

void UnicornCpu::hookMemWrite(uc_engine* uc, uc_mem_type type,
                              uint64_t address, int size, int64_t value,
                              void* user) {
    (void)uc;
    (void)type;
    UnicornCpu* self = static_cast<UnicornCpu*>(user);
    self->host_->cpuMmioWrite(static_cast<uint32_t>(address),
                              uint32_t(value), uint32_t(size));
    // The write also lands in the dummy backing RAM; harmless, because all
    // peripheral reads are computed by the model in hookMemRead.
}

bool UnicornCpu::hookFetchUnmapped(uc_engine* uc, uc_mem_type type,
                                    uint64_t address, int size, int64_t value,
                                    void* user) {
    (void)type;
    (void)size;
    (void)value;
    UnicornCpu* self = static_cast<UnicornCpu*>(user);
    // EXC_RETURN magic loaded into PC (BX LR / POP {PC} from a handler).
    // The core tried to fetch the magic value as code. Perform the REAL
    // exception return here -- pop the actual stack frame and restore the
    // interrupted registers -- instead of stopping (uc_emu_stop inside a mem
    // hook longjmps across the JIT callback frame and crashes on this build).
    // Returning false lets the engine stop the batch cleanly; runBatch sees
    // excReturnHandled_ and reports a normal continuation at the restored PC.
    if ((address & 0xFF000000ull) == 0xFF000000ull) {
        self->excReturnValue_ = uint32_t(address);
        self->popExceptionFrame();
        return false;
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "unmapped instruction fetch @0x%08llX (PC)",
                  static_cast<unsigned long long>(address));
    self->lastError_ = buf;
    return false;  // genuine fault -> stop
}

// UNICORN routes every CPU exception whose index is below EXCP_INTERRUPT
// (BKPT, SVC, and crucially the M-profile EXC_RETURN magic exit, EXCP_ =
// 8) to the UC_HOOK_INTR callback, NOT to the CPU's do_interrupt. So the
// ISR's `bx lr` (loaded LR = EXC_RETURN) shows up here. When it is a real
// Handler-mode exception return, pop the frame and continue at the
// interrupted PC -- this is genuine execution of the return.
void UnicornCpu::hookIntr(uc_engine* uc, int intno, void* user) {
    (void)uc;
    UnicornCpu* self = static_cast<UnicornCpu*>(user);
    if (intno == 8 /*EXCP_EXCEPTION_EXIT*/) {
        // The core is in Handler mode and just executed BX LR with a magic
        // EXC_RETURN value; the exception-return path in the underlying QEMU
        // is intentionally stubbed, so we perform the real frame pop ourselves.
        self->popExceptionFrame();
        return;
    }
    // Other core exception events: report for diagnostics only.
    self->host_->cpuIntrEvent(intno);
}

void UnicornCpu::hookCode(uc_engine* uc, uint64_t address, uint32_t size,
                          void* user) {
    (void)uc;
    (void)address;
    (void)size;
    UnicornCpu* self = static_cast<UnicornCpu*>(user);
    // Instruction counter only. The hook fires BEFORE each instruction
    // executes; runBatch() reports the count through executedLastBatch().
    // Execution breakpoints are deliberately NOT handled here: uc_emu_stop()
    // from this callback stops late on this unicorn build (block chaining), so
    // the Simulator runs precise host-side breakpoints one instruction at a
    // time instead -- with exact single-step mode enabled (runBreakpointBatch /
    // runOneInstructionBatch), which makes every count=1 batch exact.
    self->batchExecuted_++;
}
