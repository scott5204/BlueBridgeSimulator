#include "AgdiConfig.h"

namespace {

AgdiConfig::Data g_data;
std::wstring     g_iniPath;
std::wstring     g_sourcePath;
bool             g_loaded = false;

std::wstring LocalAppDataIni() {
    wchar_t dir[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) return std::wstring();
    std::wstring p(dir);
    p += L"\\BlueBridgeSimulator\\agdi.ini";
    return p;
}

std::wstring ReadString(const wchar_t *key, const wchar_t *def) {
    wchar_t buf[2048];
    DWORD n = GetPrivateProfileStringW(L"BlueBridge", key, def, buf, 2048, g_iniPath.c_str());
    return std::wstring(buf, n);
}

int ReadInt(const wchar_t *key, int def) {
    return (int)GetPrivateProfileIntW(L"BlueBridge", key, def, g_iniPath.c_str());
}

}  // namespace

namespace AgdiConfig {

const Data &Load() {
    if (g_loaded) return g_data;

    g_iniPath = LocalAppDataIni();

    g_data.autoStart = ReadInt(L"AutoStart", 1) != 0;
    g_data.leaveSimulatorRunning = ReadInt(L"LeaveSimulatorRunning", 1) != 0;
    g_data.connectTimeoutMs = ReadInt(L"ConnectTimeoutMs", 10000);
    g_data.requestTimeoutMs = ReadInt(L"RequestTimeoutMs", 2000);
    g_data.trace = ReadInt(L"Trace", 0) != 0;
    g_data.loadTrace = ReadInt(L"LoadTrace", 0) != 0;
    g_data.pipeName = ReadString(L"PipeName", L"");

    /* SimulatorPath: ini wins over the environment variable */
    std::wstring iniPath = ReadString(L"SimulatorPath", L"");
    if (!iniPath.empty()) {
        g_data.simulatorPath = iniPath;
        g_sourcePath = L"agdi.ini";
    } else {
        wchar_t env[MAX_PATH * 2];
        DWORD n = GetEnvironmentVariableW(L"BLUEBRIDGE_SIMULATOR", env, MAX_PATH * 2);
        if (n > 0 && n < MAX_PATH * 2) {
            g_data.simulatorPath = env;
            g_sourcePath = L"BLUEBRIDGE_SIMULATOR";
        }
    }

    g_loaded = true;
    return g_data;
}

const std::wstring &IniPath() { return g_iniPath; }
const std::wstring &SourcePath() { return g_sourcePath; }

std::wstring ResolveSimulatorPath() {
    Load();
    return g_data.simulatorPath;
}

}  // namespace AgdiConfig