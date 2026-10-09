#pragma once
/*
 * AGDI driver configuration (spec 7-2B sections 39 / 40 / 113; B.4.4 section 24).
 *
 * File:    %LOCALAPPDATA%\BlueBridgeSimulator\agdi.ini   (the one and only
 *          config location; located through the LOCALAPPDATA environment
 *          variable, never the current working directory)
 * Simulator path resolution (used from 7-2B.2 on):
 *   1. SimulatorPath= in agdi.ini
 *   2. BLUEBRIDGE_SIMULATOR environment variable
 *   3. empty -> AutoStart cannot start anything (logged)
 * (no hard-coded development path in Release builds)
 *
 * The file is read with GetPrivateProfileStringW (Win32 profile API): ANSI or
 * Unicode (UTF-16LE with BOM) files are understood; install.ps1 writes it as
 * Unicode so non-ASCII install paths survive (B.4.4 section 27, measured).
 *
 * Every field is consumed from checkpoint B.2 on; `PipeName` is an automation
 * override used by the session stress (regular runs get a unique pipe name).
 */

#include <Windows.h>
#include <string>

namespace AgdiConfig {

struct Data {
    std::wstring simulatorPath;      // SimulatorPath=
    bool         autoStart;          // AutoStart=1
    bool         leaveSimulatorRunning;  // LeaveSimulatorRunning=1
    int          connectTimeoutMs;   // ConnectTimeoutMs=10000
    int          requestTimeoutMs;   // RequestTimeoutMs=2000
    bool         trace;              // Trace=0
    bool         loadTrace;          // LoadTrace=0 (B.4.1 verbatim load trace)
    std::wstring pipeName;           // PipeName= (optional fixed pipe for
                                     // automated session stress; default empty
                                     // -> a unique name per session, section 43)
};

/* Loads the ini (defaults per spec when missing) and remembers the path that
 * was used so later stages can write it back. */
const Data &Load();
const std::wstring &IniPath();
const std::wstring &SourcePath();   // text of which source won for SimulatorPath

/* Real simulator path: ini value, else BLUEBRIDGE_SIMULATOR, else empty. */
std::wstring ResolveSimulatorPath();

}  // namespace AgdiConfig