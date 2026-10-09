#pragma once

#include <atomic>
#include <string>

#include "cpu/ICpuEngine.h"
#include "cpu/UnicornApi.h"

// ============================================================================
// Cortex-M4 CPU engine built on Unicorn (QEMU TCG).
//
//  - CPU model: QEMU "cortex-m4" (ARMv7-M + Thumb-2 + VFPv4)
//  - Regular memory (flash / SRAM) is mapped directly onto the SoC backing
//    buffers, so the CPU executes at native TCG speed there.
//  - The whole peripheral space is mapped as dummy RAM and intercepted by
//    memory hooks:
//      * UC_HOOK_MEM_READ  (before read)  -> host computes the value, which
//        is written into guest memory so the CPU observes it
//      * UC_HOOK_MEM_WRITE (before write) -> forwarded to the SoC model
//  - Exceptions (SysTick/NVIC IRQs) are injected manually between batches:
//    the engine exposes register/stack access for building exception frames.
//  - Exception return (BX LR with EXC_RETURN magic) results in an unmapped
//    fetch which is caught and reported as CpuStopReason::ExcReturn.
// ============================================================================
class UnicornCpu : public ICpuEngine {
public:
    ~UnicornCpu() override;

    bool start(ICpuHost* host) override;
    void shutdown() override;

    uint32_t getReg(CpuReg reg) override;
    void setReg(CpuReg reg, uint32_t value) override;
    bool readRegister(CpuReg reg, uint32_t& value) override;
    bool writeRegister(CpuReg reg, uint32_t value) override;
    bool setCpacr(uint32_t value) override;

    bool mapMemory(uint64_t base, size_t size, uint32_t perms,
                   void* backing) override;

    CpuStopReason runBatch(uint32_t pc, uint32_t insnCount) override;
    uint32_t executedLastBatch() const override { return executedLastBatch_; }

    // The EXC_RETURN magic value from the last unmapped fetch
    // (valid when runBatch returned CpuStopReason::ExcReturn).
    uint32_t lastExcReturn() const { return excReturnValue_; }

    // Enter/leave ARMv7-M Handler mode by writing v7m.exception directly.
    // This is the only way to make IPSR read a real non-zero value (the
    // APC/APS/ACTIPS fields of IPSR/XPSR are architecturally read-only from
    // the external register interface). exc==0 leaves Handler mode (Thread).
    void setV7mException(int exc) override;

    // Exact single-step mode (UC_CTL_EXACT_SINGLE_STEP): every translated
    // block is limited to one guest instruction and never chains, so
    // runBatch(pc, 1) executes exactly one instruction. Toggling flushes the
    // translation cache; state is cached here so repeated calls are free.
    bool setExactSingleStep(bool enable) override;

    void requestStop() override;

    bool writeGuest(uint64_t addr, const void* data, size_t size) override;
    bool readGuest(uint64_t addr, void* data, size_t size) override;
    bool invalidateRegion(uint64_t base, uint64_t end) override;

    std::string lastError() const override { return lastError_; }

    // Address ranges hooked for MMIO (STM32 peripheral space + PPB).
    static constexpr uint64_t kMmioLo = 0x40000000ull;
    static constexpr uint64_t kMmioHi = 0xE003FFFFull;

private:
    static void hookMemRead(uc_engine* uc, uc_mem_type type,
                            uint64_t address, int size, int64_t value,
                            void* user);
    static void hookMemWrite(uc_engine* uc, uc_mem_type type,
                             uint64_t address, int size, int64_t value,
                             void* user);
    static bool hookFetchUnmapped(uc_engine* uc, uc_mem_type type,
                                  uint64_t address, int size, int64_t value,
                                  void* user);
    static void hookIntr(uc_engine* uc, int intno, void* user);
    static void hookCode(uc_engine* uc, uint64_t address, uint32_t size,
                         void* user);

    static int mapReg(CpuReg reg);

    // Pop the real 8-word exception stack frame the simulator pushed on entry
    // and restore r0-r3, r12, lr, xpsr, sp and pc. Called synchronously from
    // the EXC_RETURN / unmapped-magic-fetch hooks, so the ISR return is a real
    // in-core register/memory operation (no host-side emulation of the return).
    void popExceptionFrame();

    UnicornApi* api_ = nullptr;
    uc_engine* uc_ = nullptr;
    ICpuHost* host_ = nullptr;

    uc_hook hRead_ = 0;
    uc_hook hWrite_ = 0;
    uc_hook hFetch_ = 0;
    uc_hook hIntr_ = 0;
    uc_hook hCode_ = 0;

    std::atomic<bool> stopRequested_{false};

    // per-batch state (engine thread only)
    bool excReturnPending_ = false;
    uint32_t excReturnValue_ = 0;
    // set when a real exception frame was popped in a hook (ISR returned);
    // runBatch then reports CountReached instead of Fault.
    bool excReturnHandled_ = false;
    bool intrStop_ = false;
    uint32_t executedLastBatch_ = 0;
    uint32_t batchExecuted_ = 0;

    int cpacrMode_ = 0;  // 0 = unknown, 1 = C1_C0_2, 2 = CP_REG
    bool exactStep_ = false;  // engine-side exact single-step mode state
    std::string lastError_;
};
