#pragma once
/*
 * Shared test helpers between the probe's main suite (probe_main.cpp) and the
 * B.3 run-control suite (probe_runcontrol.cpp).
 */

#include "IpcClient.h"
#include "IpcTypes.h"

/* test output (defined in probe_main.cpp) */
void Check(bool ok, const char *what);
void Info(const char *fmt, ...);

/* B.3 run-control / breakpoint / event suite (stage 7-2B.3 sections 60-72).
 * `fullStress` runs the 10000x hit-continue and 1000x run/halt concurrency
 * loops; reduced rounds still exercise every semantic path. */
bool RunControlSuite(bbx::IpcClient &c, const bbx::TargetCaps &caps,
                     bool fullStress);

/* Reads the test_debug debug map (fixed flash offset 0x200) and normalises the
 * label addresses. Returns false when the magic/end markers do not match. */
struct DebugMap {
    uint32_t resetHandler = 0;
    uint32_t step1 = 0;
    uint32_t step2 = 0;
    uint32_t step3 = 0;
    uint32_t step4 = 0;
    uint32_t loopTop = 0;
    uint32_t scratch = 0;      // RAM address used by step_4
    uint32_t counter = 0;      // RAM loop counter (str r2,[r1,#4])
};

bool ReadDebugMap(bbx::IpcClient &c, const bbx::TargetCaps &caps, DebugMap &out);