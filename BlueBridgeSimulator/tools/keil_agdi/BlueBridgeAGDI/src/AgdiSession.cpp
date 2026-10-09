/*
 * BlueBridgeAGDI <-> simulator session (stage 7-2B.2 sections 41-47).
 */

#include "AgdiSession.h"

#include <stdio.h>

#include "AgdiConfig.h"
#include "AgdiLog.h"

namespace {

bbx::IpcClient g_client;
std::atomic<bool> g_connected{false};
std::atomic<bool> g_lostLogged{false};
std::atomic<bool> g_intentionalDisconnect{false};

std::wstring g_pipeName;
bbx::TargetCaps g_caps;
uint32_t g_sessionId = 0;

PROCESS_INFORMATION g_pi;
bool g_launched = false;

/* --------------------------------------------------------------------------
 * "[IPC] ..." sink -> the AGDI log (section 81)
 * ------------------------------------------------------------------------ */
void IpcSink(const char *line) {
    const char *body = line;
    if (strncmp(body, "[IPC] ", 6) == 0) body += 6;
    size_t n = strlen(body);
    while (n > 0 && (body[n - 1] == '\r' || body[n - 1] == '\n')) --n;
    AgdiLog::WriteIpc("%.*s", (int)n, body);
}

std::wstring DirName(const std::wstring &p) {
    const size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? std::wstring() : p.substr(0, i);
}

/* Unique pipe per session so several µVision instances never collide
 * (section 43): \\.\pipe\BlueBridgeSimulator.Debug.<UV4PID>.<nonce> */
std::wstring MakeUniquePipeName() {
    wchar_t name[160];
    const unsigned long long nonce =
        (GetTickCount64() & 0xFFFFFFull) ^ ((unsigned long long)rand() << 12);
    _snwprintf_s(name, _TRUNCATE,
                 L"\\\\.\\pipe\\BlueBridgeSimulator.Debug.%lu.%llX",
                 (unsigned long)GetCurrentProcessId(), nonce);
    return name;
}

std::wstring WinErr(DWORD e) {
    wchar_t *msg = NULL;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, e, 0, (LPWSTR)&msg, 0, NULL);
    std::wstring s;
    if (n != 0 && msg != NULL) {
        s.assign(msg, n);
        while (!s.empty() && (s[s.size() - 1] == L'\r' || s[s.size() - 1] == L'\n'))
            s.erase(s.size() - 1);
        LocalFree(msg);
    } else {
        wchar_t buf[64];
        _snwprintf_s(buf, _TRUNCATE, L"error %lu", (unsigned long)e);
        s = buf;
    }
    return s;
}

/* CreateProcessW (Unicode, section 25/44): bluesim.exe --debug-pipe <name>
 * --wait-debugger. The process handle is kept for diagnostics only -- the
 * simulator is NOT killed on disconnect (LeaveSimulatorRunning=1). */
bool LaunchSimulator(const std::wstring &exe, const std::wstring &pipe) {
    std::wstring cmd = L"\"" + exe + L"\" --debug-pipe " + pipe + L" --wait-debugger";
    if (AgdiConfig::Load().trace) cmd += L" --debug-ipc-trace";

    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&g_pi, 0, sizeof(g_pi));
    const std::wstring wd = DirName(exe);
    if (!CreateProcessW(exe.c_str(), &cmd[0], NULL, NULL, FALSE, 0, NULL,
                        wd.empty() ? NULL : wd.c_str(), &si, &g_pi)) {
        const DWORD e = GetLastError();
        AgdiLog::WriteIpc("autostart FAILED CreateProcessW('%s') : %s",
                          bbx::ToUtf8(exe.c_str()).c_str(),
                          bbx::ToUtf8(WinErr(e).c_str()).c_str());
        return false;
    }
    g_launched = true;
    CloseHandle(g_pi.hThread);
    g_pi.hThread = NULL;
    AgdiLog::WriteIpc("autostart launched simulator pid=%lu exe=\"%s\"",
                      (unsigned long)g_pi.dwProcessId,
                      bbx::ToUtf8(exe.c_str()).c_str());
    return true;
}

bool ValidateCapabilities(const bbx::TargetCaps &caps) {
    bool ok = true;
    if (caps.architecture.find("Cortex-M4") == std::string::npos) {
        AgdiLog::WriteIpc("capability check FAILED: architecture \"%s\"",
                          caps.architecture.c_str());
        ok = false;
    }
    if (caps.endian != bbipc::kWireEndianLittle) {
        AgdiLog::WriteIpc("capability check FAILED: endian=%lu",
                          (unsigned long)caps.endian);
        ok = false;
    }
    if (caps.flashBase != 0x08000000u || caps.flashSize != 128u * 1024u) {
        AgdiLog::WriteIpc("capability check FAILED: flash=0x%08lX/%lu",
                          (unsigned long)caps.flashBase,
                          (unsigned long)caps.flashSize);
        ok = false;
    }
    const uint32_t required = bbipc::kCapExactSingleStep |
                              bbipc::kCapExecutionBreakpoint | bbipc::kCapFpu |
                              bbipc::kCapFlashProgramming |
                              bbipc::kCapAsyncStopEvent |
                              bbipc::kCapBatchRegisterRead;
    if ((caps.flags & required) != required) {
        AgdiLog::WriteIpc("capability check FAILED: flags=0x%08lX missing=0x%08lX",
                          (unsigned long)caps.flags,
                          (unsigned long)(required & ~caps.flags));
        ok = false;
    }
    return ok;
}

void FinishFailedConnect(const char *what) {
    AgdiLog::WriteIpc("session connect failed at %s", what);
    g_client.disconnect();
    g_connected = false;
}

}  // namespace

namespace AgdiSession {

bool Connect() {
    if (Usable()) return true;

    const AgdiConfig::Data &cfg = AgdiConfig::Load();
    bbx::SetLogFn(&IpcSink);
    g_client.setRequestTimeoutMs(cfg.requestTimeoutMs);
    g_lostLogged = false;
    g_intentionalDisconnect = false;

    if (!cfg.pipeName.empty()) {
        g_pipeName = cfg.pipeName;   // automated session stress override
    } else {
        g_pipeName = MakeUniquePipeName();
    }

    /* 1. is a simulator already listening? (short grace period) */
    if (!g_client.connect(g_pipeName, 1000)) {
        if (!cfg.autoStart) {
            AgdiLog::WriteIpc("no simulator on pipe=%ls and AutoStart=0",
                              g_pipeName.c_str());
            return false;
        }
        const std::wstring exe = AgdiConfig::ResolveSimulatorPath();
        if (exe.empty()) {
            AgdiLog::WriteIpc("AutoStart=1 but no SimulatorPath (agdi.ini) and no "
                              "BLUEBRIDGE_SIMULATOR environment variable");
            return false;
        }
        if (!LaunchSimulator(exe, g_pipeName)) return false;
        if (!g_client.connect(g_pipeName, cfg.connectTimeoutMs)) {
            FinishFailedConnect("WaitNamedPipe (connect timeout)");
            return false;
        }
    }

    /* 2. HELLO */
    bbx::HelloInfo hello;
    bbx::RequestResult r = g_client.hello(bbipc::kClientTypeKeilAgdi, GetCurrentProcessId());
    if (!r.ok() || !bbx::parseHello(r.payload, hello)) {
        FinishFailedConnect("HELLO");
        return false;
    }
    if (hello.serverMajor != bbipc::kVersionMajor) {
        AgdiLog::WriteIpc("protocol major mismatch: server %u.%u, driver %u.%u",
                          hello.serverMajor, hello.serverMinor, bbipc::kVersionMajor,
                          bbipc::kVersionMinor);
        FinishFailedConnect("HELLO version");
        return false;
    }
    g_sessionId = hello.sessionId;
    AgdiLog::WriteIpc("HELLO session=%lu server=%lu target=%s board=%s arch=%s",
                      (unsigned long)hello.sessionId, (unsigned long)hello.serverPid,
                      hello.targetName.c_str(), hello.boardName.c_str(),
                      hello.architecture.c_str());

    /* 3. capabilities (runtime truth, never hard-coded -- section 44) */
    bbx::TargetCaps caps;
    r = g_client.getCapabilities(&caps);
    if (!r.ok()) {
        FinishFailedConnect("GET_CAPABILITIES");
        return false;
    }
    AgdiLog::WriteIpc("capabilities flags=0x%08lX flash=0x%08lX/%luB sram=0x%08lX/%luB "
                      "ccm=0x%08lX/%luB maxMem=%lu",
                      (unsigned long)caps.flags, (unsigned long)caps.flashBase,
                      (unsigned long)caps.flashSize, (unsigned long)caps.sramBase,
                      (unsigned long)caps.sramSize, (unsigned long)caps.ccmBase,
                      (unsigned long)caps.ccmSize,
                      (unsigned long)caps.maxMemoryTransfer);
    if (!ValidateCapabilities(caps)) {
        FinishFailedConnect("capability validation");
        return false;
    }

    g_caps = caps;
    g_connected = true;
    return true;
}

void Disconnect() {
    g_intentionalDisconnect = true;   // teardown, not a connection loss
    if (g_client.connected() || g_connected.load()) g_client.disconnect();
    if (g_launched) {
        /* LeaveSimulatorRunning: keep the GUI (and its flash image) alive. */
        if (g_pi.hProcess != NULL) CloseHandle(g_pi.hProcess);
        memset(&g_pi, 0, sizeof(g_pi));
        g_launched = false;
        AgdiLog::WriteIpc("simulator left running (LeaveSimulatorRunning=1)");
    }
    g_connected = false;
}

bool Usable() {
    if (!g_connected.load()) return false;
    if (!g_client.connected()) {
        if (!g_lostLogged.exchange(true)) {
            AgdiLog::WriteIpc("connection lost: %s (target access now fails; "
                              "Keil keeps running)",
                              bbx::ToUtf8(g_client.lastErrorCopy().c_str()).c_str());
        }
        g_connected = false;
        return false;
    }
    return true;
}

bool Lost() { return !Usable(); }

bool IntentionalDisconnect() { return g_intentionalDisconnect.load(); }

bbx::IpcClient &Client() { return g_client; }
const bbx::TargetCaps &Caps() { return g_caps; }
uint32_t SessionId() { return g_sessionId; }
const std::wstring &PipeName() { return g_pipeName; }

uint32_t ResetHalt() {
    if (!Usable()) return bbx::kStConnectionLost;
    const bbx::RequestResult r = g_client.resetHalt();
    if (r.status == bbipc::kStOk) {
        AgdiLog::Write("AGDI", "Reset -> IPC RESET_HALT ok");
    } else if (r.lost()) {
        AgdiLog::WriteIpc("RESET_HALT lost the connection");
    } else {
        AgdiLog::Write("AGDI", "RESET_HALT status=%u", r.status);
    }
    return r.status;
}

uint32_t Step() {
    if (!Usable()) return bbx::kStConnectionLost;
    const bbx::RequestResult r = g_client.step();
    if (r.status == bbipc::kStOk) {
        AgdiLog::WriteRateLimited("AGDI", "Step -> IPC STEP ok");
    } else if (r.lost()) {
        AgdiLog::WriteIpc("STEP lost the connection");
    } else {
        AgdiLog::Write("AGDI", "STEP status=%u", r.status);
    }
    return r.status;
}

}  // namespace AgdiSession