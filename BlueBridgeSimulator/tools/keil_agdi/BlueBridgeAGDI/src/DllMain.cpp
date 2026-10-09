/*
 * BlueBridgeAGDI - DllMain (spec 7-2B section 17).
 *
 * Keep this tiny: loader lock forbids CreateProcess / pipe I/O / threads /
 * Keil callbacks / config parsing / COM. We only remember the module handle,
 * disable thread callbacks, and record one pending log line in memory (file
 * I/O happens later, on the first real AGDI call).
 */

#include <Windows.h>

#include "AgdiLog.h"

/* Module handle of this DLL (recorded in DllMain, see spec 7-2B section 17). */
HMODULE g_hModule = NULL;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    (void)reserved;
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            g_hModule = hModule;
            DisableThreadLibraryCalls(hModule);
            AgdiLog::Pending("DLL loaded (Build %s %s) module=%p", __DATE__, __TIME__, hModule);
            break;

        case DLL_PROCESS_DETACH:
            AgdiLog::Write("DllMain", "DLL_PROCESS_DETACH (reason=%lu)", (unsigned long)reason);
            AgdiLog::Shutdown();
            break;

        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
        default:
            break;
    }
    return TRUE;
}