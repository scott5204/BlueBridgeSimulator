/*
 * BlueBridgeAGDIProbe.exe -- standalone IPC diagnostic tool (stage 7-2B.2,
 * sections 5/6/24-39/80/86).
 *
 * Why it exists: it proves "MSVC x86 client <-> named pipe <-> MinGW64
 * bluesim.exe" WITHOUT µVision in the loop. If Keil's register window ever
 * misbehaves, a passing probe means the IPC layer is fine and the bug is in the
 * AGDI mapping -- and vice versa.
 *
 * It reuses exactly the same IpcClient / IpcProtocol / wire constants as
 * BlueBridgeAGDI.dll, uses no AGDI API and never links simulator code.
 *
 * Test suite (one "round"): HELLO, PING x10000, GET_CAPABILITIES, PROGRAM
 * test_debug, GET_STATE, registers (single + batch), register write/readback,
 * memory (flash vector, SRAM, CCM, flash-write rejection), RESET_HALT, 1000
 * instruction steps (requestId/event desync check), disconnect/reconnect.
 *
 * Build: tools\keil_agdi\build_agdi.ps1 (also builds the DLL).
 */

#include <Windows.h>

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "IpcClient.h"
#include "IpcProtocol.h"
#include "IpcTypes.h"
#include "probe_common.h"

/* --------------------------------------------------------------------------
 * tiny test harness (external linkage: also used by probe_runcontrol.cpp)
 * ------------------------------------------------------------------------ */
int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const char *what) {
    ++g_checks;
    if (ok) {
        std::printf("    [ok]   %s\n", what);
    } else {
        ++g_failed;
        std::printf("    [FAIL] %s\n", what);
    }
}

void Info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::printf("    ...    ");
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
}

namespace {

int g_suppressedEvents = 0;

void LogSink(const char *line) {
    /* one TARGET_STOPPED line per instruction step would drown the report;
     * the queue is verified separately (section 8 of the suite). */
    if (std::strstr(line, "[IPC] event ") != NULL) {
        ++g_suppressedEvents;
        return;
    }
    std::printf("  %s", line);
    std::fflush(stdout);
}

std::string WideToUtf8(const std::wstring &w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0,
                                NULL, NULL);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) {
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL,
                            NULL);
    }
    return s;
}

bool FileExistsW(const std::wstring &p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring DirName(const std::wstring &p) {
    const size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? std::wstring() : p.substr(0, i);
}

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH * 2);
    return std::wstring(buf, n);
}

std::wstring Join(const std::wstring &a, const std::wstring &b) {
    if (a.empty()) return b;
    std::wstring s = a;
    if (s[s.size() - 1] != L'\\' && s[s.size() - 1] != L'/') s += L'\\';
    return s + b;
}

std::wstring UpDirs(const std::wstring &base, int levels, const wchar_t *tail) {
    std::wstring p = base;
    for (int i = 0; i < levels; ++i) p = DirName(p);
    return Join(p, tail);
}

/* --------------------------------------------------------------------------
 * Intel HEX -> (address, bytes) ranges
 * ------------------------------------------------------------------------ */
struct HexRange {
    uint32_t address;
    std::vector<uint8_t> data;
};

bool HexNibble(char c, uint8_t &v) {
    if (c >= '0' && c <= '9') { v = (uint8_t)(c - '0'); return true; }
    if (c >= 'A' && c <= 'F') { v = (uint8_t)(c - 'A' + 10); return true; }
    if (c >= 'a' && c <= 'f') { v = (uint8_t)(c - 'a' + 10); return true; }
    return false;
}

bool ParseHexFile(const std::wstring &path, std::vector<HexRange> &out,
                  std::string &err) {
    FILE *f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        err = "cannot open " + WideToUtf8(path);
        return false;
    }
    uint32_t base = 0;
    char line[600];
    int lineNo = 0;
    while (std::fgets(line, sizeof(line), f)) {
        ++lineNo;
        size_t len = std::strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = 0;
        }
        if (len == 0) continue;
        if (line[0] != ':') {
            char buf[128];
            _snprintf_s(buf, _TRUNCATE, "line %d: missing ':'", lineNo);
            err = buf;
            fclose(f);
            return false;
        }
        std::vector<uint8_t> b;
        for (size_t i = 1; i + 1 < len; i += 2) {
            uint8_t hi = 0, lo = 0;
            if (!HexNibble(line[i], hi) || !HexNibble(line[i + 1], lo)) {
                char buf[128];
                _snprintf_s(buf, _TRUNCATE, "line %d: bad hex digit", lineNo);
                err = buf;
                fclose(f);
                return false;
            }
            b.push_back((uint8_t)((hi << 4) | lo));
        }
        if (b.size() < 5) {
            err = "record too short";
            fclose(f);
            return false;
        }
        const uint8_t count = b[0];
        const uint32_t addr =
            ((uint32_t)b[1] << 8) | (uint32_t)b[2];
        const uint8_t type = b[3];
        if ((size_t)count + 5 != b.size()) {
            err = "record length does not match byte count";
            fclose(f);
            return false;
        }
        uint8_t sum = 0;
        for (size_t i = 0; i < b.size(); ++i) sum = (uint8_t)(sum + b[i]);
        if (sum != 0) {
            char buf[128];
            _snprintf_s(buf, _TRUNCATE, "line %d: checksum error", lineNo);
            err = buf;
            fclose(f);
            return false;
        }
        if (type == 0x00) {
            HexRange r;
            r.address = base + addr;
            r.data.assign(b.begin() + 4, b.begin() + 4 + count);
            out.push_back(r);
        } else if (type == 0x01) {
            break;   // EOF
        } else if (type == 0x04) {
            if (count != 2) {
                err = "bad extended-linear-address record";
                fclose(f);
                return false;
            }
            base = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16);
        } else {
            Info("hex: ignoring record type %u", type);
        }
    }
    fclose(f);
    return !out.empty();
}

/* --------------------------------------------------------------------------
 * simulator launch
 * ------------------------------------------------------------------------ */
struct Launch {
    PROCESS_INFORMATION pi;
    bool started = false;
};

std::wstring MakePipeName(const wchar_t *tag) {
    wchar_t name[160];
    const unsigned long long nonce =
        (GetTickCount64() & 0xFFFFFFull) ^ ((unsigned long long)rand() << 12);
    _snwprintf_s(name, _TRUNCATE, L"\\\\.\\pipe\\BlueBridgeSimulator.Debug.%s.%lu.%llX",
                 tag, (unsigned long)GetCurrentProcessId(), nonce);
    return name;
}

bool LaunchSimulator(const std::wstring &exe, const std::wstring &pipeName,
                     Launch &out, std::string &err) {
    std::wstring cmd = L"\"" + exe + L"\" --debug-pipe " + pipeName +
                       L" --wait-debugger";
    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&out.pi, 0, sizeof(out.pi));
    const std::wstring wd = DirName(exe);
    if (!CreateProcessW(exe.c_str(), &cmd[0], NULL, NULL, FALSE, 0, NULL,
                        wd.empty() ? NULL : wd.c_str(), &si, &out.pi)) {
        err = "CreateProcessW failed for " + WideToUtf8(exe) + " (error " +
              std::to_string(GetLastError()) + ")";
        return false;
    }
    out.started = true;
    Info("launched simulator pid=%lu pipe=%ls", (unsigned long)out.pi.dwProcessId,
         pipeName.c_str());
    return true;
}

void StopSimulator(Launch &l) {
    if (!l.started) return;
    TerminateProcess(l.pi.hProcess, 0);
    WaitForSingleObject(l.pi.hProcess, 5000);
    CloseHandle(l.pi.hThread);
    CloseHandle(l.pi.hProcess);
    l.started = false;
    Info("simulator process terminated (probe-owned)");
}

/* --------------------------------------------------------------------------
 * individual probe tests -- each returns true when everything matched
 * ------------------------------------------------------------------------ */

bool TestHello(bbx::IpcClient &c, bbx::HelloInfo &hello) {
    std::printf("  [1] HELLO\n");
    bbx::RequestResult r = c.hello(bbipc::kClientTypeTest, GetCurrentProcessId());
    if (!r.ok()) {
        Check(false, "HELLO answered Ok");
        Info("status=%u", r.status);
        return false;
    }
    if (!bbx::parseHello(r.payload, hello)) {
        Check(false, "HELLO payload parses");
        return false;
    }
    char buf[256];
    _snprintf_s(buf, _TRUNCATE, "protocol %u.%u (server pid %lu, session %lu)",
                hello.serverMajor, hello.serverMinor,
                (unsigned long)hello.serverPid, (unsigned long)hello.sessionId);
    Check(hello.serverMajor == bbipc::kVersionMajor, buf);
    _snprintf_s(buf, _TRUNCATE, "target '%s' board '%s' arch '%s'",
                hello.targetName.c_str(), hello.boardName.c_str(),
                hello.architecture.c_str());
    Check(!hello.targetName.empty() && !hello.architecture.empty(), buf);
    Check(hello.sessionId != 0, "sessionId != 0");
    return true;
}

void TestPing(bbx::IpcClient &c) {
    std::printf("  [2] PING x10000\n");
    const int kCount = 10000;
    const ULONGLONG t0 = GetTickCount64();
    int bad = 0;
    for (int i = 0; i < kCount; ++i) {
        const uint64_t cookie = 0xC0FFEE0000000000ull | (uint64_t)i;
        bbx::RequestResult r = c.ping(cookie);
        uint64_t echo = 0;
        if (!r.ok() || !bbx::parseU64(r.payload, echo) || echo != cookie) {
            ++bad;
            if (bad <= 3) Info("ping %d: status=%u echo=0x%llX", i, r.status, echo);
        }
    }
    const ULONGLONG dt = GetTickCount64() - t0;
    char buf[160];
    _snprintf_s(buf, _TRUNCATE, "%d/%d cookies echoed, avg RTT %.3f ms",
                kCount - bad, kCount, double(dt) / kCount);
    Check(bad == 0, buf);
    (void)buf;
}

bool TestCapabilities(bbx::IpcClient &c, bbx::TargetCaps &caps) {
    std::printf("  [3] GET_CAPABILITIES\n");
    bbx::RequestResult r = c.getCapabilities(&caps);
    if (!r.ok()) {
        Check(false, "GET_CAPABILITIES answered Ok");
        return false;
    }
    Info("arch=%s flags=0x%08lX flash=0x%08lX/%luKiB alias=0x%08lX/%luKiB "
         "sram=0x%08lX/%luKiB ccm=0x%08lX/%luKiB maxMem=%lu",
         caps.architecture.c_str(), (unsigned long)caps.flags,
         (unsigned long)caps.flashBase, (unsigned long)(caps.flashSize / 1024),
         (unsigned long)caps.flashAliasBase,
         (unsigned long)(caps.flashAliasSize / 1024),
         (unsigned long)caps.sramBase, (unsigned long)(caps.sramSize / 1024),
         (unsigned long)caps.ccmBase, (unsigned long)(caps.ccmSize / 1024),
         (unsigned long)caps.maxMemoryTransfer);

    Check(caps.architecture.find("Cortex-M4") != std::string::npos,
          "architecture is ARM Cortex-M4F");
    Check(caps.endian == bbipc::kWireEndianLittle, "endian = little");
    Check(caps.flashBase == 0x08000000u && caps.flashSize == 128u * 1024u,
          "flash = 0x08000000 / 128 KiB");
    Check(caps.flashAliasBase == 0x00000000u && caps.flashAliasSize == 128u * 1024u,
          "alias = 0x00000000 / 128 KiB");
    Check(caps.sramBase == 0x20000000u && caps.sramSize == 32u * 1024u,
          "SRAM = 0x20000000 / 32 KiB");
    Check(caps.ccmBase == 0x10000000u && caps.ccmSize == 16u * 1024u,
          "CCM = 0x10000000 / 16 KiB");

    struct { uint32_t bit; const char *name; } required[] = {
        {bbipc::kCapExactSingleStep, "ExactStep"},
        {bbipc::kCapExecutionBreakpoint, "ExecBreakpoint"},
        {bbipc::kCapFpu, "FPU"},
        {bbipc::kCapFlashProgramming, "FlashProgramming"},
        {bbipc::kCapAsyncStopEvent, "AsyncStopEvent"},
        {bbipc::kCapBatchRegisterRead, "BatchRegisterRead"},
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        char buf[96];
        _snprintf_s(buf, _TRUNCATE, "capability %s", required[i].name);
        Check((caps.flags & required[i].bit) != 0, buf);
    }
    Check((caps.flags & bbipc::kCapDataWatchpoint) == 0,
          "capability DataWatchpoint = false");
    Check(caps.maxMemoryTransfer == 64u * 1024u, "maxMemoryTransfer = 64 KiB");
    Check(caps.maxPayload == 1024u * 1024u, "maxPayload = 1 MiB");
    return true;
}

bool ProgramHex(bbx::IpcClient &c, const std::vector<HexRange> &ranges,
                const bbx::TargetCaps &caps, uint32_t &sp, uint32_t &pc) {
    std::printf("  [4] PROGRAM test_debug (virtual flash, 7-2A transaction)\n");

    bbx::TargetState st0;
    bbx::RequestResult st = c.getState(&st0);
    if (!st.ok()) {
        Check(false, "GET_STATE before PROGRAM_BEGIN");
        return false;
    }
    if (st0.state != bbipc::kWireStateHalted) {
        Info("target not halted (state=%u) -- halting before PROGRAM_BEGIN",
             st0.state);
        c.halt();
    }
    uint64_t token = 0;
    uint32_t fbase = 0, fsize = 0;
    bbx::RequestResult r = c.programBegin(&token, &fbase, &fsize);
    if (!r.ok()) {
        Info("PROGRAM_BEGIN status=%u (target must be halted)", r.status);
        Check(false, "PROGRAM_BEGIN");
        return false;
    }
    Check(token != 0 && fbase == caps.flashBase && fsize == caps.flashSize,
          "PROGRAM_BEGIN token != 0, flash geometry echoed");

    r = c.programErase(token, fbase, fsize);
    Check(r.ok(), "PROGRAM_ERASE whole flash (staging)");
    if (!r.ok()) { c.programAbort(token); return false; }

    size_t written = 0;
    for (size_t i = 0; i < ranges.size(); ++i) {
        r = c.programWrite(token, ranges[i].address, ranges[i].data.data(),
                           ranges[i].data.size());
        if (!r.ok()) {
            Info("PROGRAM_WRITE @0x%08lX status=%u",
                 (unsigned long)ranges[i].address, r.status);
            Check(false, "PROGRAM_WRITE range");
            c.programAbort(token);
            return false;
        }
        written += ranges[i].data.size();
    }
    Info("programmed %u ranges, %u bytes", (unsigned)ranges.size(),
         (unsigned)written);
    Check(true, "PROGRAM_WRITE all ranges");

    uint32_t isp = 0, ipc = 0;
    r = c.programEnd(token, &isp, &ipc);
    if (!r.ok()) {
        Info("PROGRAM_END status=%u payload=%u bytes", r.status,
             (unsigned)r.payload.size());
        Check(false, "PROGRAM_END commit");
        return false;
    }
    sp = isp;
    pc = ipc;
    char buf[160];
    _snprintf_s(buf, _TRUNCATE, "PROGRAM_END committed, new vector SP=0x%08lX PC=0x%08lX",
                (unsigned long)sp, (unsigned long)pc);
    Check(true, buf);
    (void)buf;
    return true;
}

void TestState(bbx::IpcClient &c, const bbx::TargetCaps &caps, uint32_t vecSp,
               uint32_t vecPc) {
    std::printf("  [5] GET_STATE\n");
    bbx::TargetState s;
    bbx::RequestResult r = c.getState(&s);
    if (!r.ok()) {
        Check(false, "GET_STATE answered Ok");
        return;
    }
    Info("state=%u stopReason=%u pc=0x%08lX cycles=%llu firmwareLoaded=%u",
         s.state, s.stopReason, (unsigned long)s.pc, s.virtualCycles,
         s.firmwareLoaded);
    Check(s.state == bbipc::kWireStateHalted, "target is Halted");
    Check(s.firmwareLoaded == 1, "firmwareLoaded = 1 after PROGRAM_END");
    Check(s.pc >= caps.flashBase && s.pc < caps.flashBase + caps.flashSize,
          "PC is a legal flash address");
    Check((s.pc & 1u) == 0, "GET_STATE PC is normalised (bit0 clear)");
    Check(s.pc == (vecPc & ~1u), "GET_STATE PC equals the new vector's reset PC");
    Check(s.stopReason == bbipc::kWireStopReset, "stopReason = Reset after PROGRAM_END");
    (void)vecSp;
}

bool TestRegisters(bbx::IpcClient &c) {
    std::printf("  [6] registers (single + batch, widths, FPU)\n");
    static const uint32_t kCore[] = {
        bbipc::kWireRegR0,  bbipc::kWireRegR1,  bbipc::kWireRegR2,
        bbipc::kWireRegR3,  bbipc::kWireRegR4,  bbipc::kWireRegR5,
        bbipc::kWireRegR6,  bbipc::kWireRegR7,  bbipc::kWireRegR8,
        bbipc::kWireRegR9,  bbipc::kWireRegR10, bbipc::kWireRegR11,
        bbipc::kWireRegR12, bbipc::kWireRegSP,  bbipc::kWireRegLR,
        bbipc::kWireRegPC,  bbipc::kWireRegXPSR,
    };
    static const uint32_t kSpecial[] = {
        bbipc::kWireRegMSP,       bbipc::kWireRegPSP,
        bbipc::kWireRegPRIMASK,   bbipc::kWireRegBASEPRI,
        bbipc::kWireRegFAULTMASK, bbipc::kWireRegCONTROL,
    };

    int bad = 0;
    for (size_t i = 0; i < sizeof(kCore) / sizeof(kCore[0]); ++i) {
        uint64_t v = 0;
        bbx::RequestResult r = c.readRegister(kCore[i], &v);
        if (!r.ok()) {
            ++bad;
            Info("READ_REGISTER %s status=%u", bbx::registerName(kCore[i]),
                 r.status);
        } else if (kCore[i] <= bbipc::kWireRegR12 || kCore[i] == bbipc::kWireRegXPSR) {
            // 32-bit registers must not leak 64-bit garbage
            if ((v >> 32) != 0) {
                ++bad;
                Info("%s = 0x%llX (upper half not zero)", bbx::registerName(kCore[i]), v);
            }
        }
    }
    Check(bad == 0, "single READ_REGISTER R0-R12/SP/LR/PC/xPSR");

    bad = 0;
    for (size_t i = 0; i < sizeof(kSpecial) / sizeof(kSpecial[0]); ++i) {
        uint64_t v = 0;
        bbx::RequestResult r = c.readRegister(kSpecial[i], &v);
        if (!r.ok()) {
            ++bad;
            Info("READ_REGISTER %s status=%u", bbx::registerName(kSpecial[i]),
                 r.status);
        }
    }
    Check(bad == 0, "single READ_REGISTER MSP/PSP/PRIMASK/BASEPRI/FAULTMASK/CONTROL");

    bad = 0;
    {
        uint64_t v = 0;
        for (uint32_t id = bbipc::kWireRegS0; id <= bbipc::kWireRegS31; ++id) {
            bbx::RequestResult r = c.readRegister(id, &v);
            if (!r.ok()) ++bad;
        }
        bbx::RequestResult r = c.readRegister(bbipc::kWireRegFPSCR, &v);
        if (!r.ok()) ++bad;
    }
    Check(bad == 0, "single READ_REGISTER S0-S31 + FPSCR");

    // batch: one request for the whole core set (section 32)
    std::vector<uint32_t> ids(kCore, kCore + sizeof(kCore) / sizeof(kCore[0]));
    std::vector<bbx::RegisterValue> batch;
    bbx::RequestResult r = c.readRegisters(ids, &batch);
    Check(r.ok() && batch.size() == ids.size(),
          "READ_REGISTERS returns one entry per requested id");
    if (r.ok() && batch.size() == ids.size()) {
        int mismatched = 0;
        for (size_t i = 0; i < ids.size(); ++i) {
            uint64_t v = 0;
            bbx::RequestResult one = c.readRegister(ids[i], &v);
            if (!one.ok() || batch[i].id != ids[i] ||
                batch[i].status != bbipc::kStOk || batch[i].value != v) {
                ++mismatched;
                Info("batch[%u] %s: 0x%llX vs single 0x%llX (status %u)",
                     (unsigned)i, bbx::registerName(ids[i]), batch[i].value, v,
                     batch[i].status);
            }
        }
        Check(mismatched == 0, "batch values match single reads");
    }

    // write / readback / restore (section 31)
    uint64_t saved = 0;
    r = c.readRegister(bbipc::kWireRegR0, &saved);
    Check(r.ok(), "snapshot R0");
    r = c.writeRegister(bbipc::kWireRegR0, 0x12345678ull);
    Check(r.ok(), "WRITE_REGISTER R0 = 0x12345678");
    uint64_t back = 0;
    r = c.readRegister(bbipc::kWireRegR0, &back);
    char buf[128];
    _snprintf_s(buf, _TRUNCATE, "R0 readback = 0x%llX", back);
    Check(r.ok() && back == 0x12345678ull, buf);
    r = c.writeRegister(bbipc::kWireRegR0, saved);
    Check(r.ok(), "R0 restored");
    (void)buf;
    return true;
}

void TestMemory(bbx::IpcClient &c, const bbx::TargetCaps &caps, uint32_t vecSp) {
    std::printf("  [7] memory (flash vector / SRAM / CCM / flash write reject)\n");

    // flash vector table
    std::vector<uint8_t> vec;
    bbx::RequestResult r = c.readMemory(caps.flashBase, 8, &vec);
    Check(r.ok() && vec.size() == 8, "READ_MEMORY 0x08000000 (vector table)");
    if (r.ok() && vec.size() == 8) {
        uint32_t sp = (uint32_t)vec[0] | ((uint32_t)vec[1] << 8) |
                      ((uint32_t)vec[2] << 16) | ((uint32_t)vec[3] << 24);
        uint32_t pc = (uint32_t)vec[4] | ((uint32_t)vec[5] << 8) |
                      ((uint32_t)vec[6] << 16) | ((uint32_t)vec[7] << 24);
        char buf[160];
        _snprintf_s(buf, _TRUNCATE, "vector[0]=SP=0x%08lX vector[1]=PC=0x%08lX",
                    (unsigned long)sp, (unsigned long)pc);
        Check(true, buf);
        (void)buf;
        Check(sp >= caps.sramBase && sp <= caps.sramBase + caps.sramSize,
              "vector SP lies in SRAM");
        Check(pc >= caps.flashBase && pc < caps.flashBase + caps.flashSize,
              "vector reset PC lies in flash");
        Check(vecSp == sp, "register SP matches vector[0]");
    }

    // alias read must mirror the flash
    std::vector<uint8_t> viaAlias;
    r = c.readMemory(caps.flashAliasBase, 8, &viaAlias);
    Check(r.ok() && viaAlias.size() == 8 && viaAlias == vec,
          "alias 0x00000000 mirrors flash 0x08000000");

    // SRAM + CCM pattern write/readback/restore
    struct { const char *name; uint32_t addr; } areas[] = {
        {"SRAM 0x20000000..+32K", caps.sramBase + 0x1000},
        {"CCM  0x10000000..+16K", caps.ccmBase + 0x1000},
    };
    for (size_t a = 0; a < sizeof(areas) / sizeof(areas[0]); ++a) {
        std::vector<uint8_t> orig;
        r = c.readMemory(areas[a].addr, 16, &orig);
        if (!r.ok()) {
            char buf[128];
            _snprintf_s(buf, _TRUNCATE, "read %s", areas[a].name);
            Check(false, buf);
            continue;
        }
        uint8_t pattern[16];
        for (int i = 0; i < 16; ++i) pattern[i] = (uint8_t)(0xDE - i);
        r = c.writeMemory(areas[a].addr, pattern, 16);
        {
            char buf[128];
            _snprintf_s(buf, _TRUNCATE, "WRITE_MEMORY %s", areas[a].name);
            Check(r.ok(), buf);
        }
        std::vector<uint8_t> back;
        r = c.readMemory(areas[a].addr, 16, &back);
        {
            char buf[128];
            _snprintf_s(buf, _TRUNCATE, "readback %s matches pattern", areas[a].name);
            Check(r.ok() && back.size() == 16 &&
                      memcmp(back.data(), pattern, 16) == 0,
                  buf);
        }
        c.writeMemory(areas[a].addr, orig.data(), orig.size());
    }

    // ordinary WRITE_MEMORY must never touch the virtual flash (section 52)
    std::vector<uint8_t> before;
    r = c.readMemory(caps.flashBase + 0x100, 4, &before);
    Check(r.ok() && before.size() == 4, "read flash word before rejected write");
    uint8_t junk[4] = {0x11, 0x22, 0x33, 0x44};
    r = c.writeMemory(caps.flashBase + 0x100, junk, 4);
    char buf[160];
    _snprintf_s(buf, _TRUNCATE, "WRITE_MEMORY to flash rejected with Unsupported (got %u)",
                r.status);
    Check(r.status == bbipc::kStUnsupported, buf);
    (void)buf;
    std::vector<uint8_t> after;
    r = c.readMemory(caps.flashBase + 0x100, 4, &after);
    Check(r.ok() && after == before, "flash readback unchanged");
}

void TestResetAndStep(bbx::IpcClient &c, const bbx::TargetCaps &caps) {
    std::printf("  [8] RESET_HALT + 1000 instruction steps\n");

    bbx::RequestResult r = c.resetHalt();
    Check(r.ok(), "RESET_HALT answered Ok");
    if (!r.ok()) return;

    std::vector<uint8_t> vec;
    r = c.readMemory(caps.flashBase, 8, &vec);
    uint32_t sp = 0, pc = 0;
    if (r.ok() && vec.size() == 8) {
        sp = (uint32_t)vec[0] | ((uint32_t)vec[1] << 8) | ((uint32_t)vec[2] << 16) |
             ((uint32_t)vec[3] << 24);
        pc = (uint32_t)vec[4] | ((uint32_t)vec[5] << 8) | ((uint32_t)vec[6] << 16) |
             ((uint32_t)vec[7] << 24);
    }
    uint64_t regSp = 0, regPc = 0;
    c.readRegister(bbipc::kWireRegSP, &regSp);
    c.readRegister(bbipc::kWireRegPC, &regPc);
    char buf[200];
    _snprintf_s(buf, _TRUNCATE,
                "SP=0x%08llX (vector 0x%08lX), PC=0x%08llX (vector 0x%08lX)",
                regSp, (unsigned long)sp, regPc, (unsigned long)(pc & ~1u));
    Check(regSp == sp && regPc == (pc & ~1u), buf);
    (void)buf;

    bbx::TargetState s0;
    r = c.getState(&s0);
    if (!r.ok()) {
        Check(false, "GET_STATE before stepping");
        return;
    }
    Check(s0.state == bbipc::kWireStateHalted, "Halted before STEP");

    // one step: stopReason must become SingleStep, cycles +1, PC in flash
    r = c.step();
    Check(r.ok(), "STEP answered Ok");

    bbx::TargetState s1;
    r = c.getState(&s1);
    _snprintf_s(buf, _TRUNCATE, "after 1 step: pc=0x%08lX cycles=%llu (was %llu)",
                (unsigned long)s1.pc, s1.virtualCycles, s0.virtualCycles);
    Check(r.ok(), buf);
    (void)buf;
    Check(s1.state == bbipc::kWireStateHalted &&
              s1.stopReason == bbipc::kWireStopSingleStep,
          "Halted / SingleStep after STEP");
    Check(s1.virtualCycles == s0.virtualCycles + 1, "cycles increased by exactly 1");
    Check(s1.pc != s0.pc, "PC moved to the real next instruction");
    Check(s1.pc >= caps.flashBase && s1.pc < caps.flashBase + caps.flashSize,
          "PC still inside flash");

    // 1000 further steps: no desync, no timeout, no stale response
    const int kSteps = 1000;
    const bbx::IpcClient::Stats st0 = c.stats();
    int bad = 0;
    for (int i = 0; i < kSteps; ++i) {
        bbx::RequestResult sr = c.step();
        if (!sr.ok()) {
            ++bad;
            if (bad <= 3) Info("step %d status=%u", i, sr.status);
        }
    }
    const bbx::IpcClient::Stats st1 = c.stats();
    _snprintf_s(buf, _TRUNCATE,
                "%d/%d steps Ok, timeouts delta=%llu stale delta=%llu protocolErrors delta=%llu",
                kSteps - bad, kSteps, st1.timeouts - st0.timeouts,
                st1.staleResponses - st0.staleResponses,
                st1.protocolErrors - st0.protocolErrors);
    Check(bad == 0, buf);
    Check(st1.timeouts == st0.timeouts && st1.staleResponses == st0.staleResponses &&
              st1.protocolErrors == st0.protocolErrors,
          "no timeout / stale response / protocol error during 1000 steps");

    bbx::TargetState s2;
    r = c.getState(&s2);
    Check(r.ok() && s2.state == bbipc::kWireStateHalted, "still Halted after 1000 steps");
    Check(s2.virtualCycles > s1.virtualCycles, "virtual cycles advanced");
    Check(s2.stopReason == bbipc::kWireStopSingleStep, "stopReason stayed SingleStep");

    // TARGET_STOPPED events must have been queued, not treated as errors
    size_t ev = 0;
    bbx::IpcEvent e;
    while (c.popEvent(e)) ++ev;
    _snprintf_s(buf, _TRUNCATE, "%u TARGET_STOPPED events consumed (%d log lines suppressed)",
                (unsigned)ev, g_suppressedEvents);
    Check(ev >= (size_t)(kSteps / 2), buf);
}

bool TestReconnect(bbx::IpcClient &c, const std::wstring &pipeName,
                   uint32_t firstSessionId) {
    std::printf("  [9] disconnect / reconnect (new session id)\n");
    c.disconnect();
    Check(!c.connected(), "disconnect() leaves the client disconnected");

    if (!c.connect(pipeName, 5000)) {
        Check(false, "reconnect to the same pipe");
        return false;
    }
    bbx::HelloInfo hello;
    bbx::RequestResult r = c.hello(bbipc::kClientTypeTest, GetCurrentProcessId());
    Check(r.ok() && bbx::parseHello(r.payload, hello), "HELLO on the new session");
    char buf[128];
    _snprintf_s(buf, _TRUNCATE, "sessionId changed: %lu -> %lu",
                (unsigned long)firstSessionId, (unsigned long)hello.sessionId);
    Check(hello.sessionId != firstSessionId && hello.sessionId != 0, buf);
    (void)buf;
    return true;
}

/* --------------------------------------------------------------------------
 * one full round
 * ------------------------------------------------------------------------ */
struct Options {
    std::wstring pipeName;     // empty -> launch our own simulator
    std::wstring simExe;
    std::wstring hexPath;
    int rounds = 1;
    bool keep = false;
    bool fullStress = false;   // force the 10000x/1000x loops in EVERY round
    bool preload = false;      // B.3 fixture: program the HEX and exit
    bool readMode = false;     // B.4.2: read live memory and exit
    bool read32 = false;       // --read32: one 32-bit word instead of a dump
    bool regsMode = false;     // B.4.3: dump the core registers and exit
    bool stateMode = false;    // B.4.3: dump the target state and exit
    bool haltMode = false;     // B.4.3: send HALT to a running target and exit
    bool resumeMode = false;   // B.4.3: send RESUME to a halted target and exit
    uint32_t readAddr = 0;
    uint32_t readLen = 0;
};

/* --------------------------------------------------------------------------
 * B.3 fixture mode: program `hexPath` into the virtual flash of a simulator and
 * leave it running (spec sections 57 / 123). The disconnect that follows is
 * exactly the "known initial state" the Keil session then starts from: the
 * server halts the target and clears its breakpoints -- no AGDI Download is
 * involved anywhere (that is B.4).
 * ------------------------------------------------------------------------ */
int RunPreload(const Options &opt) {
    bbx::SetLogFn(&LogSink);
    bbx::IpcClient client;
    client.setRequestTimeoutMs(bbipc::kRequestTimeoutMs);

    Launch launch;
    std::wstring pipe = opt.pipeName;
    if (pipe.empty()) {
        pipe = MakePipeName(L"Preload");
        std::string err;
        if (!LaunchSimulator(opt.simExe, pipe, launch, err)) {
            std::printf("preload: %s\n", err.c_str());
            return 2;
        }
    }
    if (!client.connect(pipe, 15000)) {
        std::printf("preload: connect failed: %ls\n",
                    client.lastErrorCopy().c_str());
        StopSimulator(launch);
        return 2;
    }

    bbx::HelloInfo hello;
    bbx::TargetCaps caps;
    bbx::RequestResult r = client.hello(bbipc::kClientTypeTest,
                                        GetCurrentProcessId());
    if (!r.ok() || !bbx::parseHello(r.payload, hello) ||
        !client.getCapabilities(&caps).ok()) {
        std::printf("preload: HELLO/capabilities failed\n");
        client.disconnect();
        StopSimulator(launch);
        return 2;
    }
    std::vector<HexRange> ranges;
    std::string herr;
    if (!ParseHexFile(opt.hexPath, ranges, herr)) {
        std::printf("preload: %s\n", herr.c_str());
        client.disconnect();
        StopSimulator(launch);
        return 2;
    }
    uint32_t sp = 0, pc = 0;
    if (!ProgramHex(client, ranges, caps, sp, pc)) {
        std::printf("preload: PROGRAM failed\n");
        client.disconnect();
        StopSimulator(launch);
        return 2;
    }
    /* "programmed image" proof: read the vector table back from the flash */
    std::vector<uint8_t> vec;
    if (client.readMemory(caps.flashBase, 8, &vec).ok() && vec.size() == 8) {
        const uint32_t rsp = (uint32_t)vec[0] | ((uint32_t)vec[1] << 8) |
                             ((uint32_t)vec[2] << 16) | ((uint32_t)vec[3] << 24);
        const uint32_t rpc = (uint32_t)vec[4] | ((uint32_t)vec[5] << 8) |
                             ((uint32_t)vec[6] << 16) | ((uint32_t)vec[7] << 24);
        std::printf("preload: flash vector table SP=0x%08lX PC=0x%08lX\n",
                    (unsigned long)rsp, (unsigned long)(rpc & ~1u));
    }
    client.disconnect();       // server: halt + clear breakpoints (known state)
    std::printf("preload: OK -- simulator kept running on %ls (halted, no "
                "breakpoints: the Keil session starts from a known state)\n",
                pipe.c_str());
    (void)launch;
    return 0;
}

/* --------------------------------------------------------------------------
 * Readback mode (B.4.2 sections 44/45/54/55): read live memory from a running
 * simulator and print it machine-parsable, so the download test script can
 * compare the virtual flash against the Keil HEX file record by record. It
 * never writes anything and only stops a simulator it launched itself.
 * ------------------------------------------------------------------------ */
int RunRead(const Options &opt) {
    bbx::SetLogFn(&LogSink);
    bbx::IpcClient client;
    client.setRequestTimeoutMs(bbipc::kRequestTimeoutMs);

    Launch launch;
    std::wstring pipe = opt.pipeName;
    if (pipe.empty()) {
        pipe = MakePipeName(L"Read");
        std::string err;
        if (!LaunchSimulator(opt.simExe, pipe, launch, err)) {
            std::printf("read: %s\n", err.c_str());
            return 2;
        }
    }
    if (!client.connect(pipe, 15000)) {
        std::printf("read: connect failed: %ls\n", client.lastErrorCopy().c_str());
        StopSimulator(launch);
        return 2;
    }

    bbx::HelloInfo hello;
    bbx::TargetCaps caps;
    bbx::RequestResult r = client.hello(bbipc::kClientTypeTest,
                                        GetCurrentProcessId());
    if (!r.ok() || !bbx::parseHello(r.payload, hello) ||
        !client.getCapabilities(&caps).ok()) {
        std::printf("read: HELLO/capabilities failed\n");
        client.disconnect();
        StopSimulator(launch);
        return 2;
    }

    if (opt.haltMode || opt.resumeMode) {
        /* B.4.3: stop a target that a Keil session is currently running (the
         * source-step "user presses Stop while a step-run blocks" test). */
        const bbx::RequestResult hr =
            opt.haltMode ? client.halt() : client.resume();
        std::printf("%s: status=%lu\n", opt.haltMode ? "halt" : "resume",
                    (unsigned long)hr.status);
        client.disconnect();
        StopSimulator(launch);
        return hr.ok() ? 0 : 2;
    }

    if (opt.regsMode || opt.stateMode) {
        /* B.4.3: machine-parsable single-line diagnostics for the source-level
         * acceptance script (PC/LR/SP evidence + "target halted" state). */
        if (opt.stateMode) {
            bbx::TargetState st;
            const bbx::RequestResult sr = client.getState(&st);
            if (!sr.ok()) {
                std::printf("state: GET_STATE failed status=%lu\n",
                            (unsigned long)sr.status);
                client.disconnect();
                StopSimulator(launch);
                return 2;
            }
            std::printf("state: state=%lu stopReason=%lu pc=0x%08lX\n",
                        (unsigned long)st.state, (unsigned long)st.stopReason,
                        (unsigned long)st.pc);
        }
        if (opt.regsMode) {
            static const struct { const char *name; uint32_t id; } kRegs[] = {
                {"r0", bbipc::kWireRegR0},   {"r1", bbipc::kWireRegR1},
                {"r2", bbipc::kWireRegR2},   {"r3", bbipc::kWireRegR3},
                {"r4", bbipc::kWireRegR4},   {"r5", bbipc::kWireRegR5},
                {"r6", bbipc::kWireRegR6},   {"r7", bbipc::kWireRegR7},
                {"r8", bbipc::kWireRegR8},   {"r9", bbipc::kWireRegR9},
                {"r10", bbipc::kWireRegR10}, {"r11", bbipc::kWireRegR11},
                {"r12", bbipc::kWireRegR12}, {"sp", bbipc::kWireRegSP},
                {"lr", bbipc::kWireRegLR},   {"pc", bbipc::kWireRegPC},
                {"xpsr", bbipc::kWireRegXPSR}, {"msp", bbipc::kWireRegMSP},
                {"psp", bbipc::kWireRegPSP}, {"control", bbipc::kWireRegCONTROL},
            };
            std::printf("regs:");
            for (size_t i = 0; i < sizeof(kRegs) / sizeof(kRegs[0]); ++i) {
                uint64_t v = 0;
                const bbx::RequestResult rr = client.readRegister(kRegs[i].id, &v);
                if (rr.ok()) {
                    std::printf(" %s=0x%08lX", kRegs[i].name, (unsigned long)v);
                } else {
                    std::printf(" %s=ERR%lu", kRegs[i].name,
                                (unsigned long)rr.status);
                }
            }
            std::printf("\n");
        }
        client.disconnect();
        StopSimulator(launch);
        return 0;
    }

    std::vector<uint8_t> data;
    const bbx::RequestResult m =
        client.readMemory(opt.readAddr, opt.readLen, &data);
    if (!m.ok() || data.size() != opt.readLen) {
        std::printf("read: READ_MEMORY 0x%08lX +%lu failed status=%lu\n",
                    (unsigned long)opt.readAddr, (unsigned long)opt.readLen,
                    (unsigned long)m.status);
        client.disconnect();
        StopSimulator(launch);
        return 2;
    }

    if (opt.read32) {
        const uint32_t v = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
        std::printf("read32: addr=0x%08lX value=0x%08lX\n",
                    (unsigned long)opt.readAddr, (unsigned long)v);
    } else {
        std::printf("read: base=0x%08lX len=%lu data=",
                    (unsigned long)opt.readAddr, (unsigned long)data.size());
        for (size_t i = 0; i < data.size(); ++i) {
            std::printf("%02x", data[i]);
        }
        std::printf("\n");
    }

    client.disconnect();
    StopSimulator(launch);
    return 0;
}

bool RunRound(int round, const Options &opt, std::string &err) {
    std::printf("---- round %d ----\n", round);
    g_checks = 0;
    g_failed = 0;

    bbx::SetLogFn(&LogSink);

    bbx::IpcClient client;
    client.setRequestTimeoutMs(bbipc::kRequestTimeoutMs);

    Launch launch;
    std::wstring pipe = opt.pipeName;
    if (pipe.empty()) {
        pipe = MakePipeName(L"Probe");
        if (!LaunchSimulator(opt.simExe, pipe, launch, err)) return false;
    }
    if (!client.connect(pipe, 15000)) {
        err = "connect failed: " + WideToUtf8(client.lastErrorCopy());
        StopSimulator(launch);
        return false;
    }

    bool ok = true;
    bbx::HelloInfo hello;
    bbx::TargetCaps caps;
    std::vector<HexRange> ranges;

    if (TestHello(client, hello)) {
        TestPing(client);
        if (TestCapabilities(client, caps)) {
            std::string herr;
            if (!ParseHexFile(opt.hexPath, ranges, herr)) {
                Check(false, "Intel HEX parses");
                Info("%s", herr.c_str());
                ok = false;
            } else {
                Info("hex %ls: %u ranges", opt.hexPath.c_str(),
                     (unsigned)ranges.size());
                uint32_t sp = 0, pc = 0;
                if (ProgramHex(client, ranges, caps, sp, pc)) {
                    TestState(client, caps, sp, pc);
                    TestRegisters(client);
                    TestMemory(client, caps, sp);
                    TestResetAndStep(client, caps);
                    /* B.3: run control / breakpoints / events (sections 60-72).
                     * The heavy loops run in every round only with --full-stress;
                     * otherwise round 1 uses the full sizes and later rounds the
                     * reduced ones. */
                    if (!RunControlSuite(client, caps,
                                         opt.fullStress || round == 1)) {
                        ok = false;
                    }
                } else {
                    ok = false;
                }
            }
        } else {
            ok = false;
        }
    } else {
        ok = false;
    }

    const uint32_t sessionId = hello.sessionId;
    if (ok && g_failed == 0 && sessionId != 0) {
        if (!TestReconnect(client, pipe, sessionId)) ok = false;
    }
    client.disconnect();
    StopSimulator(launch);

    std::printf("---- round %d: %s (%d checks, %d failed)\n\n", round,
                (ok && g_failed == 0) ? "PASS" : "FAIL", g_checks, g_failed);
    return ok && g_failed == 0;
}

bool ParseArgs(int argc, char **argv, Options &opt, bool &help) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::wstring &out) -> bool {
            if (i + 1 >= argc) return false;
            const std::string v = argv[++i];
            out.assign(v.begin(), v.end());   // argv is ANSI; the default paths
                                              // below cover non-ASCII layouts
            return true;
        };
        if (a == "--pipe") {
            if (!next(opt.pipeName)) return false;
        } else if (a == "--exe") {
            if (!next(opt.simExe)) return false;
        } else if (a == "--hex") {
            if (!next(opt.hexPath)) return false;
        } else if (a == "--rounds") {
            std::wstring v;
            if (!next(v)) return false;
            opt.rounds = _wtoi(v.c_str());
            if (opt.rounds < 1) opt.rounds = 1;
        } else if (a == "--keep") {
            opt.keep = true;
        } else if (a == "--full-stress") {
            opt.fullStress = true;
        } else if (a == "--preload") {
            opt.preload = true;
        } else if (a == "--read") {
            std::wstring v;
            if (!next(v)) return false;
            opt.readAddr = (uint32_t)wcstoul(v.c_str(), NULL, 0);
            if (!next(v)) return false;
            opt.readLen = (uint32_t)wcstoul(v.c_str(), NULL, 0);
            if (opt.readLen == 0) return false;
            opt.readMode = true;
        } else if (a == "--read32") {
            std::wstring v;
            if (!next(v)) return false;
            opt.readAddr = (uint32_t)wcstoul(v.c_str(), NULL, 0);
            opt.readLen = 4;
            opt.read32 = true;
            opt.readMode = true;
        } else if (a == "--regs") {
            opt.regsMode = true;
            opt.readMode = true;
        } else if (a == "--state") {
            opt.stateMode = true;
            opt.readMode = true;
        } else if (a == "--halt") {
            opt.haltMode = true;
            opt.readMode = true;
        } else if (a == "--resume") {
            opt.resumeMode = true;
            opt.readMode = true;
        } else if (a == "--help" || a == "-h") {
            help = true;
            return true;
        } else {
            std::printf("unknown argument '%s'\n", a.c_str());
            return false;
        }
    }
    return true;
}

void Usage() {
    std::printf(
        "BlueBridgeAGDIProbe -- standalone BlueBridge Debug IPC diagnostics\n"
        "\n"
        "usage: BlueBridgeAGDIProbe [--rounds N] [--pipe <name>] [--exe <bluesim.exe>]\n"
        "                           [--hex <test_debug.hex>] [--keep]\n"
        "\n"
        "  --rounds N   run the whole suite N times (stress; default 1)\n"
        "  --full-stress  run the 10000x hit/continue and 1000x run/halt loops in\n"
        "               every round (default: full sizes in round 1 only)\n"
        "  --preload    program --hex into the virtual flash and exit (B.3 Keil\n"
        "               fixture; use with --pipe to keep the simulator running)\n"
        "  --pipe NAME  connect to an already running simulator (no launch)\n"
        "  --exe PATH   simulator to launch (default: BLUEBRIDGE_SIMULATOR,\n"
        "               else ../../../../release/bluesim.exe or build/bluesim.exe)\n"
        "  --hex PATH   firmware image for the step test (default:\n"
        "               ../../../../firmware/test_debug/test_debug.hex)\n"
        "  --read A L   read L bytes at address A and print them (machine\n"
        "               parsable; B.4.2 flash readback, use with --pipe)\n"
        "  --read32 A   read the 32-bit word at address A\n"
        "  --regs       dump the core registers (r0-r12/sp/lr/pc/xpsr/msp/psp/\n"
        "               control) as one machine-parsable line (B.4.3)\n"
        "  --state      dump the target state (state/stopReason/pc) (B.4.3)\n"
        "  --halt       send HALT to a running target (B.4.3 stop-during-step)\n"
        "  --resume     send RESUME to a halted target (B.4.3)\n"
        "  --keep       leave a launched simulator running\n");
}

std::wstring ResolveSimulatorExe(const Options &opt) {
    if (!opt.simExe.empty()) return opt.simExe;

    wchar_t env[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(L"BLUEBRIDGE_SIMULATOR", env,
                                            MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2 && FileExistsW(env)) return env;

    const std::wstring exeDir = DirName(ExePath());
    const wchar_t *tails[] = {
        L"release\\bluesim.exe",
        L"build\\bluesim.exe",
        L"build\\Release\\bluesim.exe",
    };
    for (size_t i = 0; i < sizeof(tails) / sizeof(tails[0]); ++i) {
        const std::wstring p = UpDirs(exeDir, 4, tails[i]);
        if (FileExistsW(p)) return p;
    }
    return std::wstring();
}

std::wstring ResolveHexPath(const Options &opt) {
    if (!opt.hexPath.empty()) return opt.hexPath;
    const std::wstring exeDir = DirName(ExePath());
    const wchar_t *tails[] = {
        L"firmware\\test_debug\\test_debug.hex",
        L"..\\firmware\\test_debug\\test_debug.hex",
    };
    for (size_t i = 0; i < sizeof(tails) / sizeof(tails[0]); ++i) {
        const std::wstring p = UpDirs(exeDir, 4, tails[i]);
        if (FileExistsW(p)) return p;
    }
    return std::wstring();
}

}  // namespace

int main(int argc, char **argv) {
    Options opt;
    bool help = false;
    if (!ParseArgs(argc, argv, opt, help)) {
        Usage();
        return 2;
    }
    if (help) {
        Usage();
        return 0;
    }

    std::printf("BlueBridgeAGDIProbe -- MSVC x86 <-> named pipe <-> MinGW64 bluesim\n");
    std::printf("rounds=%d\n\n", opt.rounds);

    if (opt.pipeName.empty()) {
        if (opt.simExe.empty()) opt.simExe = ResolveSimulatorExe(opt);
        if (opt.simExe.empty()) {
            std::printf("error: no simulator exe found; pass --exe or set "
                        "BLUEBRIDGE_SIMULATOR\n");
            return 2;
        }
        std::printf("simulator : %ls\n", opt.simExe.c_str());
    } else {
        std::printf("pipe      : %ls (external simulator)\n", opt.pipeName.c_str());
    }

    if (opt.readMode) {
        return RunRead(opt);   // B.4.2 readback: no HEX needed
    }

    if (opt.hexPath.empty()) opt.hexPath = ResolveHexPath(opt);
    if (opt.hexPath.empty()) {
        std::printf("error: no test_debug.hex found; pass --hex\n");
        return 2;
    }
    std::printf("firmware  : %ls\n\n", opt.hexPath.c_str());

    if (opt.preload) {
        return RunPreload(opt);
    }

    int passed = 0;
    for (int r = 1; r <= opt.rounds; ++r) {
        std::string err;
        if (RunRound(r, opt, err)) {
            ++passed;
        } else {
            std::printf("round %d FAILED: %s\n", r, err.empty() ? "(checks)" : err.c_str());
            if (opt.rounds == 1) break;
        }
    }

    std::printf("==== probe result: %d/%d rounds PASS ====\n", passed, opt.rounds);
    return passed == opt.rounds ? 0 : 1;
}