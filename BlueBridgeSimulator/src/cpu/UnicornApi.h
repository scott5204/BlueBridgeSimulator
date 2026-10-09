#pragma once

#include <cstdint>

#include "unicorn/unicorn.h"

// ============================================================================
// Runtime loader for unicorn.dll (QEMU TCG CPU emulator, repackaged as a
// library). The DLL is loaded dynamically so that the simulator build is
// independent of any particular compiler/runtime used to build unicorn.
//
// This is the real QEMU CPU core executing real ARM Cortex-M4 instructions -
// no re-implementation of the ISA.
// ============================================================================
struct UnicornApi {
    bool load(const char* explicitPath = nullptr);

    bool ok() const { return loaded; }
    const char* loadError() const { return err; }

    unsigned int (*uc_version)(unsigned int* major, unsigned int* minor) = nullptr;
    uc_err (*uc_open)(uc_arch arch, uc_mode mode, uc_engine** uc) = nullptr;
    uc_err (*uc_close)(uc_engine* uc) = nullptr;
    const char* (*uc_strerror)(uc_err code) = nullptr;
    uc_err (*uc_errno)(uc_engine* uc) = nullptr;
    uc_err (*uc_reg_read)(uc_engine* uc, int regid, void* value) = nullptr;
    uc_err (*uc_reg_write)(uc_engine* uc, int regid, const void* value) = nullptr;
    uc_err (*uc_mem_map)(uc_engine* uc, uint64_t address, size_t size,
                         uint32_t perms) = nullptr;
    uc_err (*uc_mem_map_ptr)(uc_engine* uc, uint64_t address, size_t size,
                             uint32_t perms, void* ptr) = nullptr;
    uc_err (*uc_mem_write)(uc_engine* uc, uint64_t address, const void* bytes,
                           size_t size) = nullptr;
    uc_err (*uc_mem_read)(uc_engine* uc, uint64_t address, void* bytes,
                          size_t size) = nullptr;
    uc_err (*uc_emu_start)(uc_engine* uc, uint64_t begin, uint64_t until,
                           uint64_t timeout, size_t count) = nullptr;
    uc_err (*uc_emu_stop)(uc_engine* uc) = nullptr;
    uc_err (*uc_hook_add)(uc_engine* uc, uc_hook* hh, int type, void* callback,
                          void* user_data, uint64_t begin, uint64_t end,
                          ...) = nullptr;
    uc_err (*uc_hook_del)(uc_engine* uc, uc_hook hh) = nullptr;
    uc_err (*uc_ctl)(uc_engine* uc, uc_control_type control, ...) = nullptr;

    // Optional: entered/leaves ARMv7-M Handler mode by writing v7m.exception.
    // May be null if the loaded DLL lacks this custom export (older build),
    // in which case the simulator falls back to Thread-mode ISR execution.
    uc_err (*setV7mException)(uc_engine* uc, int exc) = nullptr;

private:
    bool loaded = false;
    char err[256] = {0};
};

// Global accessor; loads on first use.
UnicornApi& unicornApi();
