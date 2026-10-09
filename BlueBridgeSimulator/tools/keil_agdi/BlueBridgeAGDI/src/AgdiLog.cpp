#include "AgdiLog.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

CRITICAL_SECTION g_cs;
bool             g_csReady = false;
HANDLE           g_file = INVALID_HANDLE_VALUE;

char  g_pending[512];
bool  g_hasPending = false;

bool  g_trace = false;
bool  g_verbose = false;

/* per-second rate limiter for hot paths */
struct RateState {
    DWORD tick;        // start of the current 1-second window
    int   written;     // lines written in this window
    int   dropped;     // lines suppressed in this window
};
RateState g_rate;                  // single bucket shared by all hot paths
const int kLinesPerSecond = 50;

void EnsureInit() {
    if (g_csReady) return;
    InitializeCriticalSection(&g_cs);
    g_csReady = true;
}

void OpenLogLocked() {
    if (g_file != INVALID_HANDLE_VALUE) return;

    wchar_t dir[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) return;

    wchar_t path[MAX_PATH * 2];
    _snwprintf_s(path, _TRUNCATE, L"%s\\BlueBridgeSimulator", dir);
    CreateDirectoryW(path, NULL);
    _snwprintf_s(path, _TRUNCATE, L"%s\\BlueBridgeSimulator\\logs", dir);
    CreateDirectoryW(path, NULL);
    _snwprintf_s(path, _TRUNCATE, L"%s\\BlueBridgeSimulator\\logs\\BlueBridgeAGDI.log", dir);

    /* rotate once if the file grew too large */
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
        ULARGE_INTEGER sz;
        sz.LowPart = fad.nFileSizeLow;
        sz.HighPart = fad.nFileSizeHigh;
        if (sz.QuadPart > AgdiLog::kMaxLogBytes) {
            wchar_t bak[MAX_PATH * 2];
            _snwprintf_s(bak, _TRUNCATE, L"%s.1", path);
            DeleteFileW(bak);
            MoveFileW(path, bak);
        }
    }

    g_file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_file == INVALID_HANDLE_VALUE) return;

    /* flush anything recorded before the log was usable (DllMain) */
    if (g_hasPending) {
        DWORD written = 0;
        WriteFile(g_file, g_pending, (DWORD)strlen(g_pending), &written, NULL);
        g_hasPending = false;
    }
}

void EmitLocked(const char *line) {
    if (g_file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(g_file, line, (DWORD)strlen(line), &written, NULL);
}

void FormatLine(char *out, size_t outSize, const char *tag, const char *fn,
                const char *fmt, va_list ap) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    int head = 0;
    if (fn != NULL) {
        head = _snprintf_s(out, outSize, _TRUNCATE,
                           "%04u-%02u-%02u %02u:%02u:%02u.%03u  pid=%lu tid=%lu  [%s] %s",
                           st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                           st.wMilliseconds, (unsigned long)GetCurrentProcessId(),
                           (unsigned long)GetCurrentThreadId(), tag, fn);
    } else {
        head = _snprintf_s(out, outSize, _TRUNCATE,
                           "%04u-%02u-%02u %02u:%02u:%02u.%03u  pid=%lu tid=%lu  [%s]",
                           st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                           st.wMilliseconds, (unsigned long)GetCurrentProcessId(),
                           (unsigned long)GetCurrentThreadId(), tag);
    }
    if (head < 0) return;
    size_t used = strlen(out);
    if (used + 2 >= outSize) return;
    out[used++] = ' ';
    out[used] = 0;
    const int body = _vsnprintf_s(out + used, outSize - used, _TRUNCATE, fmt, ap);
    if (body < 0) {
        /* e.g. a %ls argument outside the C locale: keep the line visible
         * instead of silently dropping the whole message (use Utf8/ToUtf8). */
        strncpy_s(out + used, outSize - used,
                  "<argument conversion failed>", _TRUNCATE);
    }
    size_t len = strlen(out);
    if (len + 2 < outSize) {
        out[len++] = '\r';
        out[len++] = '\n';
        out[len] = 0;
    }
}

}  // namespace

namespace AgdiLog {

void Pending(const char *fmt, ...) {
    EnsureInit();
    /* no locking needed: DllMain runs before any other thread uses the DLL,
     * but take it anyway to keep the state consistent. */
    if (g_csReady) EnterCriticalSection(&g_cs);
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    FormatLine(line, sizeof(line), "AGDI", "DllMain", fmt, ap);
    va_end(ap);
    if (!g_hasPending) {
        strncpy_s(g_pending, line, _TRUNCATE);
        g_hasPending = true;
    }
    if (g_csReady) LeaveCriticalSection(&g_cs);
}

void Write(const char *fn, const char *fmt, ...) {
    EnsureInit();
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    FormatLine(line, sizeof(line), "AGDI", fn, fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g_cs);
    OpenLogLocked();
    EmitLocked(line);
    LeaveCriticalSection(&g_cs);
}

void WriteIpc(const char *fmt, ...) {
    EnsureInit();
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    FormatLine(line, sizeof(line), "IPC", NULL, fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g_cs);
    OpenLogLocked();
    EmitLocked(line);
    LeaveCriticalSection(&g_cs);
}

void WriteRateLimited(const char *fn, const char *fmt, ...) {
    if (!g_trace) return;

    EnsureInit();
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    FormatLine(line, sizeof(line), "AGDI", fn, fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g_cs);
    OpenLogLocked();

    DWORD now = GetTickCount();
    if (g_rate.tick == 0 || now - g_rate.tick >= 1000) {
        if (g_rate.dropped > 0) {
            char sum[128];
            _snprintf_s(sum, _TRUNCATE, "  ... %d call(s) suppressed in the previous second\r\n", g_rate.dropped);
            EmitLocked(sum);
        }
        g_rate.tick = now;
        g_rate.written = 0;
        g_rate.dropped = 0;
    }
    if (g_rate.written < kLinesPerSecond) {
        EmitLocked(line);
        ++g_rate.written;
    } else {
        ++g_rate.dropped;
    }
    LeaveCriticalSection(&g_cs);
}

void SetTrace(bool on) { g_trace = on; }
bool Trace() { return g_trace; }

void WriteVerbose(const char *fn, const char *fmt, ...) {
    if (!g_verbose) return;

    EnsureInit();
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    FormatLine(line, sizeof(line), "TRACE", fn, fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g_cs);
    OpenLogLocked();
    EmitLocked(line);      /* verbatim: never rate limited (B.4.1) */
    LeaveCriticalSection(&g_cs);
}

void SetVerbose(bool on) { g_verbose = on; }
bool Verbose() { return g_verbose; }

void Shutdown() {
    if (!g_csReady) return;
    EnterCriticalSection(&g_cs);
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&g_cs);
}

}  // namespace AgdiLog