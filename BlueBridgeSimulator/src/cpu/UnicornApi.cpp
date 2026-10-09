#include "cpu/UnicornApi.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

#ifdef _WIN32
using ModHandle = HMODULE;
ModHandle openModule(const char* path) { return LoadLibraryA(path); }
void* resolveSym(ModHandle m, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(m, name));
}
void closeModule(ModHandle m) { FreeLibrary(m); }
#else
using ModHandle = void*;
ModHandle openModule(const char* path) { return dlopen(path, RTLD_NOW); }
void* resolveSym(ModHandle m, const char* name) { return dlsym(m, name); }
void closeModule(ModHandle m) { dlclose(m); }
#endif

std::mutex g_mutex;
UnicornApi g_api;
bool g_tried = false;

}  // namespace

UnicornApi& unicornApi() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_tried) {
        g_tried = true;
        g_api.load(nullptr);
    }
    return g_api;
}

bool UnicornApi::load(const char* explicitPath) {
    if (loaded) return true;

    std::vector<std::string> candidates;
    if (explicitPath) candidates.push_back(explicitPath);

#ifdef _WIN32
    // 1. same directory as the running executable
    char exePath[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH)) {
        std::string exe(exePath);
        auto slash = exe.find_last_of("\\/");
        if (slash != std::string::npos)
            candidates.push_back(exe.substr(0, slash + 1) + "unicorn.dll");
    }
    candidates.emplace_back("unicorn.dll");
    // 2. project third_party layout (relative to cwd)
    candidates.emplace_back("third_party\\unicorn\\bin\\unicorn.dll");
#else
    candidates.emplace_back("unicorn.so");
    candidates.emplace_back("./third_party/unicorn/lib/libunicorn.so.2");
#endif

    ModHandle mod = nullptr;
    std::string lastErr;
    for (auto& c : candidates) {
        mod = openModule(c.c_str());
        if (mod) break;
        lastErr = c;
    }
    if (!mod) {
        std::snprintf(err, sizeof(err),
                      "unicorn library not found (tried incl. '%s')",
                      lastErr.c_str());
        return false;
    }

#define RESOLVE(field, name)                                         \
    do {                                                             \
        field = reinterpret_cast<decltype(field)>(resolveSym(mod, name)); \
        if (!field) {                                                \
            std::snprintf(err, sizeof(err),                          \
                          "missing export '%s' in unicorn library", name); \
            return false;                                            \
        }                                                            \
    } while (0)

    RESOLVE(uc_version, "uc_version");
    RESOLVE(uc_open, "uc_open");
    RESOLVE(uc_close, "uc_close");
    RESOLVE(uc_strerror, "uc_strerror");
    RESOLVE(uc_errno, "uc_errno");
    RESOLVE(uc_reg_read, "uc_reg_read");
    RESOLVE(uc_reg_write, "uc_reg_write");
    RESOLVE(uc_mem_map, "uc_mem_map");
    RESOLVE(uc_mem_map_ptr, "uc_mem_map_ptr");
    RESOLVE(uc_mem_write, "uc_mem_write");
    RESOLVE(uc_mem_read, "uc_mem_read");
    RESOLVE(uc_emu_start, "uc_emu_start");
    RESOLVE(uc_emu_stop, "uc_emu_stop");
    RESOLVE(uc_hook_add, "uc_hook_add");
    RESOLVE(uc_hook_del, "uc_hook_del");
    RESOLVE(uc_ctl, "uc_ctl");
#undef RESOLVE

    // Optional export (only present in patched / rebuilt DLLs). Not required:
    // without it the simulator falls back to Thread-mode ISR execution.
    setV7mException = reinterpret_cast<decltype(setV7mException)>(
        resolveSym(mod, "uc_arm_set_v7m_exception"));

    loaded = true;
    return true;
}
