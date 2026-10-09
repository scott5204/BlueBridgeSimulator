/*
 * BlueBridgeAGDI - AGDI entry points (stage 7-2B checkpoint B.2).
 *
 * B.2 scope: Registers / Memory / Reset / Instruction Step really drive
 * BlueBridgeSimulator through the Debug IPC of stage 7-2A. Run, Stop,
 * Breakpoints, Application Download and async TARGET_STOPPED delivery stay out
 * of the AGDI lifecycle (B.3 / B.4) -- events are only queued and logged.
 *
 * Error policy (official AGDI error codes, AGDI.H):
 *   transport gone / target unavailable -> AG_NOACCESS
 *   rejected read / write                -> AG_RDFAILED / AG_WRFAILED
 *   flash through the memory-write path  -> AG_RO (read-only, section 52)
 *   unknown operation code               -> AG_INVALOP
 * Every export catches everything: a C++ exception must never cross the DLL
 * boundary into µVision.
 */

#include "AgdiCompat.h"
#include "AgdiLog.h"
#include "AgdiConfig.h"
#include "AgdiBreakpoints.h"
#include "AgdiMemory.h"
#include "AgdiProgram.h"
#include "AgdiRegisters.h"
#include "AgdiRunControl.h"
#include "AgdiSession.h"

namespace {

/* Official COMTYP.H: message posted to the µVision main frame (handed over by
 * AG_INITPHANDLEP) using the registered message token from AG_INITUSRMSG. The
 * official sample driver uses it whenever the target connection is gone: it
 * makes µVision terminate the debug session with a communication error instead
 * of waiting forever (spec section 107 -- never fake a stop reason). */
#define BB_MSG_UV2_TERMINATE 0x0005

/* Deliberately no vectored exception handler: a dangling VEH after DLL unload
 * crashes µVision on its own (stage 7-2B.2 finding). The access paths report
 * official AGDI error codes instead. */

struct DriverState {
    bool  playDead;      // no usable IPC session
    bool  initSeen;      // AG_INITFEATURES seen
    pCBF  pCbFunc;       // AG_INITCALLBACK
    UL32 *pCurPc;        // AG_INITCURPC
    void *pDbgBlk;       // EnumUvARM7(nCode=2): struct dbgblk*
    HWND  hMainFrame;    // AG_INITPHANDLEP: pointer to the µVision main frame
    UL32  uv2Msg;        // AG_INITUSRMSG: registered message token
    AG_BP **pBpHead;     // AG_INITBPHEAD: pointer to the µVision breakpoint list
    bool  configLoaded;
};

DriverState g_state = { true, false, NULL, NULL, NULL, NULL, 0, NULL, false };

void LoadConfigOnce() {
    if (g_state.configLoaded) return;
    g_state.configLoaded = true;
    const AgdiConfig::Data &cfg = AgdiConfig::Load();
    AgdiLog::SetTrace(cfg.trace);
    AgdiLog::SetVerbose(cfg.loadTrace);
    AgdiLog::Write("Config",
                   "ini=\"%s\" trace=%d loadTrace=%d autoStart=%d leaveRunning=%d connectTimeout=%d requestTimeout=%d simPathSource=\"%s\"",
                   bbx::ToUtf8(AgdiConfig::IniPath().c_str()).c_str(),
                   cfg.trace ? 1 : 0, cfg.loadTrace ? 1 : 0,
                   cfg.autoStart ? 1 : 0,
                   cfg.leaveSimulatorRunning ? 1 : 0, cfg.connectTimeoutMs, cfg.requestTimeoutMs,
                   bbx::ToUtf8(AgdiConfig::SourcePath().empty() ? L"(none)"
                                                                : AgdiConfig::SourcePath().c_str()).c_str());
}

const char *InitItemName(U16 sub) {
    switch (sub) {
        case AG_INITMENU:       return "AG_INITMENU";
        case AG_INITEXTDLGUPD:  return "AG_INITEXTDLGUPD";
        case AG_INITMHANDLEP:   return "AG_INITMHANDLEP";
        case AG_INITPHANDLEP:   return "AG_INITPHANDLEP";
        case AG_INITINSTHANDLE: return "AG_INITINSTHANDLE";
        case AG_INITBPHEAD:     return "AG_INITBPHEAD";
        case AG_INITCURPC:      return "AG_INITCURPC";
        case AG_INITDOEVENTS:   return "AG_INITDOEVENTS";
        case AG_INITUSRMSG:     return "AG_INITUSRMSG";
        case AG_INITCALLBACK:   return "AG_INITCALLBACK";
        case AG_INITFLASHLOAD:  return "AG_INITFLASHLOAD";
        case AG_STARTFLASHLOAD: return "AG_STARTFLASHLOAD";
        case AG_INITCWINAPP:    return "AG_INITCWINAPP";
        case AG_INITSTARTLOAD:  return "AG_INITSTARTLOAD";
        case AG_INITENDLOAD:    return "AG_INITENDLOAD";
        default:                return "AG_INITITEM(unknown)";
    }
}

const char *ExecItemName(U16 sub) {
    switch (sub) {
        case AG_UNINIT:       return "AG_UNINIT";
        case AG_RESET:        return "AG_RESET";
        case AG_GETMODE:      return "AG_GETMODE";
        case AG_RUNSTART:     return "AG_RUNSTART";
        case AG_RUNSTOP:      return "AG_RUNSTOP";
        case AG_QUERY_LASIG:  return "AG_QUERY_LASIG";
        case AG_KILLED_LASIG: return "AG_KILLED_LASIG";
        default:              return "AG_EXECITEM(unknown)";
    }
}

const char *FeatureName(U16 sub) {
    switch (sub) {
        case AG_F_MEMACCR:   return "AG_F_MEMACCR";
        case AG_F_REGACCR:   return "AG_F_REGACCR";
        case AG_F_TRACE:     return "AG_F_TRACE";
        case AG_F_COVERAGE:  return "AG_F_COVERAGE";
        case AG_F_PALYZE:    return "AG_F_PALYZE";
        case AG_F_MEMMAP:    return "AG_F_MEMMAP";
        case AG_F_RESETR:    return "AG_F_RESETR";
        case AG_F_LANALYZER: return "AG_F_LANALYZER";
        default:             return "AG_F_?(unknown)";
    }
}

/* Official AGDI.H names for the AG_BpInfo nCodes (documented once, used in the
 * log so the recorded µVision call sequence is readable). */
const char *BpInfoName(U16 nCode) {
    switch (nCode) {
        case AG_BPQUERY:    return "AG_BPQUERY";
        case AG_BPTOGGLE:   return "AG_BPTOGGLE";
        case AG_BPINSREM:   return "AG_BPINSREM";
        case AG_BPACTIVATE: return "AG_BPACTIVATE";
        case AG_BPDISALL:   return "AG_BPDISALL";
        case AG_BPKILLALL:  return "AG_BPKILLALL";
        case AG_BPEXQUERY:  return "AG_BPEXQUERY";
        case AG_BPENABLE:   return "AG_BPENABLE";
        case AG_BPDISABLE:  return "AG_BPDISABLE";
        case AG_BPKILL:     return "AG_BPKILL";
        case AG_BPSET:      return "AG_BPSET";
        case AG_CADRVALID:  return "AG_CADRVALID";
        default:            return "AG_BP?(unknown)";
    }
}

/* Official AGDI.H names for the AG_GoStep nCodes. */
const char *GoStepName(U16 nCode) {
    switch (nCode) {
        case AG_STOPRUN:  return "AG_STOPRUN";
        case AG_NSTEP:    return "AG_NSTEP";
        case AG_GOTILADR: return "AG_GOTILADR";
        case AG_GOFORBRK: return "AG_GOFORBRK";
        default:          return "AG_GO?(unknown)";
    }
}

/* Keil "Reset" arrives as AG_Init(AG_EXECITEM|AG_RESET) -- exactly like the
 * official sample, which then asks µVision to disassemble at the new $. */
void HandleReset() {
    if (!AgdiSession::Usable()) {
        AgdiLog::Write("AG_Init", "AG_RESET ignored (no session)");
        return;
    }
    const uint32_t st = AgdiSession::ResetHalt();
    if (st != bbipc::kStOk) {
        AgdiLog::Write("AG_Init", "AG_RESET failed status=%u", st);
        return;
    }
    AgdiRegisters::Invalidate();
    AgdiRegisters::Refresh();          // also updates *pCURPC
    if (g_state.pCbFunc != NULL) {
        static char cmd[] = "U $\n";   // official sample AGDI.CPP
        g_state.pCbFunc(AG_CB_EXECCMD, cmd);
    }
}

/* Post-execution bookkeeping shared by Run and Step (official AGDI.CPP does
 * GetRegs() + *pCURPC = <new PC> after every Go/Step): drop the register cache,
 * re-read from the target and let µVision know where the PC is. */
void RefreshAfterExecution() {
    AgdiRegisters::Invalidate();
    AgdiRegisters::Refresh();
}

/* Connection lost while the target was running: terminate the debug session in
 * µVision (official MSG_UV2_TERMINATE mechanism) instead of pretending the
 * target stopped (sections 106/107). */
void TerminateLostSession(const char *where) {
    if (AgdiSession::IntentionalDisconnect()) {
        AgdiLog::Write("AGDI", "%s: session ended by the driver (no terminate "
                       "message)", where);
        return;
    }
    AgdiLog::Write("AGDI", "%s: connection lost while running -> terminating the "
                   "Keil debug session (MSG_UV2_TERMINATE)", where);
    if (g_state.hMainFrame != NULL && g_state.uv2Msg != 0) {
        PostMessageW(g_state.hMainFrame, g_state.uv2Msg, BB_MSG_UV2_TERMINATE, 0);
    }
}

/* Body of AG_GoStep. Kept separate from the exported function so the B.4.1
 * application-load trace can record entry/exit + return value on every exit
 * path without touching the execution state machine itself. */
U32 GoStepImpl(U16 nCode, U32 nSteps, GADR *pA) {
    const unsigned tid = (unsigned)GetCurrentThreadId();

    if (g_state.playDead || !AgdiSession::Usable()) {
        AgdiLog::WriteRateLimited("AG_GoStep", "tid=%lu nCode=0x%02X(%s) -> 0 "
                                  "(disconnected)", (unsigned long)tid,
                                  (unsigned)nCode, GoStepName(nCode));
        return 0;   // official disconnected answer
    }

    switch (nCode) {
        case AG_STOPRUN: {
            /* Official: "force target to stop Go/Step", called from the
             * µVision Stop thread while a Go/Step call is blocked. */
            const U32 nE = AgdiRunControl::Stop();
            AgdiLog::Write("AG_GoStep", "tid=%lu AG_STOPRUN -> %lu (%s)", 
                           (unsigned long)tid, (unsigned long)nE,
                           nE == 1 ? "stopped" : "still executing");
            if (nE == 1) {
                /* the target is halted now; the blocked Go call refreshes the
                 * registers when its own wait returns */
                AgdiRegisters::Invalidate();
            }
            return nE;
        }

        case AG_NSTEP: {
            /* Synchronous official semantics: n instruction steps. */
            uint32_t lastErr = bbipc::kStOk;
            U32 done = 0;
            for (U32 i = 0; i < nSteps; ++i) {
                const uint32_t st = AgdiSession::Step();
                if (st != bbipc::kStOk) {
                    lastErr = st;
                    break;
                }
                ++done;
            }
            RefreshAfterExecution();   // also updates *pCURPC
            AgdiLog::WriteRateLimited("AG_GoStep", "tid=%lu AG_NSTEP %lu -> %lu "
                                      "step(s), err=%u", (unsigned long)tid,
                                      (unsigned long)nSteps, (unsigned long)done,
                                      lastErr);
            if (lastErr != bbipc::kStOk) {
                return (lastErr == bbx::kStConnectionLost ||
                        lastErr == bbx::kStNotConnected)
                           ? AG_NOACCESS
                           : AG_INVALOP;
            }
            return AG_OK;
        }

        case AG_GOFORBRK:
        case AG_GOTILADR: {
            uint32_t tempAddr = AgdiRunControl::kNoTemporaryAddress;
            if (nCode == AG_GOTILADR) {
                if (pA == NULL) {
                    AgdiLog::Write("AG_GoStep", "tid=%lu AG_GOTILADR without "
                                   "address -> AG_INVALOP", (unsigned long)tid);
                    return AG_INVALOP;
                }
                /* temporary execution breakpoint at the requested address
                 * (refcounted with any user breakpoint on it, removed in
                 * every outcome -- section 48) */
                tempAddr = AgdiBreakpoints::Canonical((uint32_t)pA->Adr);
            }
            AgdiLog::Write("AG_GoStep", "tid=%lu %s start%s", (unsigned long)tid,
                           GoStepName(nCode),
                           nCode == AG_GOTILADR ? " (temporary bp)" : "");
            const AgdiRunControl::Outcome out =
                AgdiRunControl::Run(0 /* no RPC-timeout: execution wait */,
                                    tempAddr);
            if (out.accepted && !out.lost) {
                RefreshAfterExecution();   // also updates *pCURPC
            }
            AgdiLog::Write("AG_GoStep", "tid=%lu %s -> %s", (unsigned long)tid,
                           GoStepName(nCode),
                           out.stopped ? "stopped" : (out.lost ? "lost" : "no-stop"));
            if (out.lost) {
                TerminateLostSession("AG_GoStep");
                return 0;   // official sample: run ends, session is terminated
            }
            return 0;       // nE = 0 when the run ended normally
        }

        default:
            AgdiLog::WriteRateLimited("AG_GoStep", "tid=%lu nCode=0x%02X "
                                      "(unknown) nSteps=%lu -> 0",
                                      (unsigned long)tid, (unsigned)nCode,
                                      (unsigned long)nSteps);
            return 0;
    }
}

}  // namespace

extern "C" {

/* --------------------------------------------------------------------------
 * Family check / session entry (first calls µVision makes)
 * ------------------------------------------------------------------------ */

int _EXPO_ EnumUvARM7(void *p, DWORD nCode) {
    try {
        LoadConfigOnce();
        if (nCode == 2) {
            /* p = struct dbgblk* (COMTYP.H); return the ARM family id 7 exactly
             * like the official sample driver does. */
            g_state.pDbgBlk = p;
            AgdiLog::Write("EnumUvARM7", "nCode=2 dbgblk=%p -> return 7 (ARM)", p);
            return 7;
        }
        AgdiLog::Write("EnumUvARM7", "nCode=%lu -> return 0", (unsigned long)nCode);
        return 0;
    } catch (...) {
        AgdiLog::Write("EnumUvARM7", "EXCEPTION -> return 0");
        return 0;
    }
}

int _EXPO_ DllUv3Cap(DWORD nCode, void *p) {
    try {
        LoadConfigOnce();
        switch (nCode) {
            case 2:  /* match CPU family */
                AgdiLog::Write("DllUv3Cap", "nCode=2 -> return 7 (ARM)");
                return 7;
            case 1:  /* Cpu/Target-DLL settings; B.2: no dialog (agdi.ini instead) */
                AgdiLog::Write("DllUv3Cap", "nCode=1 (settings) p=%p -> return 0 (no dialog)", p);
                return 0;
            default:
                AgdiLog::Write("DllUv3Cap", "nCode=%lu -> return 0", (unsigned long)nCode);
                return 0;
        }
    } catch (...) {
        AgdiLog::Write("DllUv3Cap", "EXCEPTION -> return 0");
        return 0;
    }
}

/* --------------------------------------------------------------------------
 * AG_Init - feature/item handshake, target start, reset, teardown
 * ------------------------------------------------------------------------ */

U32 __cdecl AG_Init(U16 nCode, void *vp) {
    try {
        LoadConfigOnce();
        switch (nCode & 0xFF00) {
            case AG_INITFEATURES: {
                g_state.initSeen = true;
                if (vp != NULL) {
                    SUPP *supp = (SUPP *)vp;
                    /* no access while running, no trace/coverage/PA/memmap/LA */
                    supp->MemAccR = 0;
                    supp->RegAccR = 0;
                    supp->hTrace = 0;
                    supp->hCover = 0;
                    supp->hPaLyze = 0;
                    supp->hMemMap = 0;
                    supp->ResetR = 0;
                    supp->ExtBrk = 0;
                    supp->LaSupp = 0;
                }
                /* official "Initialize & start the target": here -- and only
                 * here -- the IPC session is created (never in DllMain). */
                if (AgdiSession::Connect()) {
                    g_state.playDead = false;
                    AgdiRegisters::Install(g_state.pCbFunc, g_state.pCurPc);
                    AgdiProgram::ResetSession();   // a new session always starts Idle
                    AgdiLog::Write("AG_Init", "AG_INITFEATURES -> connected session=%lu",
                                   (unsigned long)AgdiSession::SessionId());
                    return AG_OK;
                }
                /* No session: the driver stays loaded and every target call
                 * answers AG_NOACCESS (exactly B.1's proven "target
                 * unavailable" behaviour). The official CMSIS_AGDI returns the
                 * init failure code here, which makes µVision cancel the
                 * driver; in an automated (-j0) session the follow-up dialog is
                 * invisible and hangs the run, so the verified B.1 semantics are
                 * kept on purpose. */
                g_state.playDead = true;
                AgdiLog::Write("AG_Init", "AG_INITFEATURES -> no IPC session: target "
                               "unavailable, driver keeps answering AG_NOACCESS");
                return AG_OK;
            }

            case AG_GETFEATURE:
                /* official AGDI.CPP answers the capability bit recorded in
                 * AG_INITFEATURES; B.3 keeps every optional capability off,
                 * so unknown codes answer 0 (the official sample's default). */
                AgdiLog::WriteRateLimited("AG_Init", "AG_GETFEATURE %s (0x%04X) -> 0",
                                          FeatureName(nCode & 0x00FF), (unsigned)nCode);
                return 0;

            case AG_INITITEM: {
                U16 sub = (U16)(nCode & 0x00FF);
                if (sub == AG_INITCALLBACK) {
                    g_state.pCbFunc = (pCBF)vp;
                    AgdiLog::Write("AG_Init", "AG_INITITEM AG_INITCALLBACK pCbFunc=%p", vp);
                } else if (sub == AG_INITCURPC) {
                    g_state.pCurPc = (UL32 *)vp;
                    AgdiLog::Write("AG_Init", "AG_INITITEM AG_INITCURPC pCURPC=%p", vp);
                } else if (sub == AG_INITPHANDLEP) {
                    /* official AGDI.CPP: "pointer to parent handle (MainFrame)" */
                    g_state.hMainFrame = (HWND)vp;
                    AgdiLog::Write("AG_Init", "AG_INITITEM AG_INITPHANDLEP hMainFrame=%p", vp);
                } else if (sub == AG_INITUSRMSG) {
                    /* official AGDI.CPP: "Registered Message for SendMessage" */
                    g_state.uv2Msg = (UL32)(ULONG_PTR)vp;
                    AgdiLog::Write("AG_Init", "AG_INITITEM AG_INITUSRMSG token=0x%08lX",
                                   (unsigned long)g_state.uv2Msg);
                } else if (sub == AG_INITBPHEAD) {
                    /* official AGDI.CPP: "pointer to head of Bp-List" */
                    g_state.pBpHead = (AG_BP **)vp;
                    AgdiLog::Write("AG_Init", "AG_INITITEM AG_INITBPHEAD pBpHead=%p", vp);
                } else if (sub == AG_INITSTARTLOAD) {
                    /* 'Load about to start', official item := LOADPARMS*
                     * (AGDI.H). B.4.1: record verbatim what µVision announces
                     * here -- file, incremental flag and especially NoCode
                     * (a NOCODE load is symbols only and must never write the
                     * virtual flash). B.4.2: this opens the programming
                     * transaction through AgdiProgram. */
                    if (vp != NULL) {
                        const LOADPARMS *lp = (const LOADPARMS *)vp;
                        AgdiLog::Write("AG_Init", "AG_INITITEM AG_INITSTARTLOAD "
                                       "vp=%p file=\"%.400s\" incremental=%u noCode=%u",
                                       vp, lp->szFile, (unsigned)lp->Incremental,
                                       (unsigned)lp->NoCode);
                        const U32 nE = AgdiProgram::BeginLoad(lp);
                        if (nE != AG_OK) return nE;   // official error to µVision
                    } else {
                        AgdiLog::Write("AG_Init", "AG_INITITEM AG_INITSTARTLOAD vp=NULL");
                        const U32 nE = AgdiProgram::BeginLoad(NULL);
                        if (nE != AG_OK) return nE;
                    }
                } else if (sub == AG_INITENDLOAD) {
                    /* 'Load finished' (official doc says vp := NULL; the local
                     * µVision 5.43 sends the LOADPARMS pointer again).
                     * B.4.2: commits an active,
                     * healthy transaction (PROGRAM_END). */
                    AgdiLog::Write("AG_Init", "AG_INITITEM AG_INITENDLOAD vp=%p", vp);
                    const U32 nE = AgdiProgram::EndLoad();
                    AgdiRegisters::Invalidate();   // commit resets+halts the target
                    if (nE != AG_OK) return nE;
                } else {
                    AgdiLog::Write("AG_Init", "AG_INITITEM %s (0x%04X) vp=%p",
                                   InitItemName(sub), sub, vp);
                }
                return AG_OK;
            }

            case AG_EXECITEM: {
                U16 sub = (U16)(nCode & 0x00FF);
                if (sub == AG_UNINIT) {
                    /* official teardown: reader joined, simulator stays alive.
                     * Local breakpoint registry and run state are dropped --
                     * the simulator clears its own breakpoints on disconnect,
                     * so nothing may survive into the next session (41/89).
                     * B.4.2: a still-open load transaction is aborted first
                     * (needs the live session for PROGRAM_ABORT). */
                    AgdiProgram::ResetSession();
                    AgdiSession::Disconnect();
                    g_state.playDead = true;
                    AgdiRegisters::Invalidate();
                    AgdiBreakpoints::ResetSession();
                    AgdiRunControl::ResetSession();
                    AgdiLog::Write("AG_Init", "AG_EXECITEM AG_UNINIT -> IPC disconnected");
                } else if (sub == AG_RESET) {
                    HandleReset();
                } else {
                    AgdiLog::Write("AG_Init", "AG_EXECITEM %s (0x%04X) vp=%p",
                                   ExecItemName(sub), sub, vp);
                }
                return AG_OK;
            }

            default:
                /* official sample keeps nE = 0 for unknown items */
                AgdiLog::Write("AG_Init", "unknown nCode=0x%04X vp=%p -> 0", nCode, vp);
                return 0;
        }
    } catch (...) {
        AgdiLog::Write("AG_Init", "EXCEPTION nCode=0x%04X -> AG_INVALOP", nCode);
        return AG_INVALOP;
    }
}

/* --------------------------------------------------------------------------
 * Register access
 * ------------------------------------------------------------------------ */

U32 __cdecl AG_AllReg(U16 nCode, void *vp) {
    try {
        const bool vt = AgdiLog::Verbose();
        const U32 nE = AgdiRegisters::AllReg(nCode, vp);
        if (vt) {
            AgdiLog::WriteVerbose("AG_AllReg", "nCode=0x%02X vp=%p -> %lu",
                                  (unsigned)nCode, vp, (unsigned long)nE);
        }
        return nE;
    } catch (...) {
        AgdiLog::Write("AG_AllReg", "EXCEPTION");
        return AG_NOACCESS;
    }
}

U32 __cdecl AG_RegAcc(U16 nCode, U32 nReg, GVAL *pV) {
    try {
        const bool vt = AgdiLog::Verbose();
        const U32 nE = AgdiRegisters::RegAcc(nCode, nReg, pV);
        if (vt) {
            AgdiLog::WriteVerbose("AG_RegAcc", "nCode=0x%02X nReg=%lu pV=%p -> %lu",
                                  (unsigned)nCode, (unsigned long)nReg, pV,
                                  (unsigned long)nE);
        }
        return nE;
    } catch (...) {
        AgdiLog::Write("AG_RegAcc", "EXCEPTION");
        return AG_NOACCESS;
    }
}

/* --------------------------------------------------------------------------
 * Memory access
 * ------------------------------------------------------------------------ */

U32 __cdecl AG_MemAcc(U16 nCode, UC8 *pB, GADR *pA, UL32 nMany) {
    try {
        const UL32 adr = (pA != NULL) ? pA->Adr : 0;
        const bool vt = AgdiLog::Verbose();
        const U32 nE = AgdiMemory::MemAcc(nCode, pB, pA, nMany);
        if (vt) {
            AgdiLog::WriteVerbose("AG_MemAcc",
                                  "nCode=0x%02X(%s) adr=0x%08lX many=%lu "
                                  "errAdr=0x%08lX -> %lu",
                                  (unsigned)nCode, AgdiMemory::MemCodeName(nCode),
                                  (unsigned long)adr, (unsigned long)nMany,
                                  (unsigned long)(pA != NULL ? pA->ErrAdr : 0),
                                  (unsigned long)nE);
        }
        return nE;
    } catch (...) {
        AgdiLog::Write("AG_MemAcc", "EXCEPTION");
        return AG_NOACCESS;
    }
}

U32 __cdecl AG_MemAtt(U16 nCode, UL32 nAttr, GADR *pA) {
    try {
        const UL32 adr = (pA != NULL) ? pA->Adr : 0;
        const UL32 len = (pA != NULL) ? pA->nLen : 0;
        const bool vt = AgdiLog::Verbose();
        const U32 nE = AgdiMemory::MemAtt(nCode, nAttr, pA);
        if (vt) {
            AgdiLog::WriteVerbose("AG_MemAtt",
                                  "nCode=0x%02X nAttr=0x%08lX adr=0x%08lX nLen=%lu "
                                  "retAdr=0x%08lX -> %lu",
                                  (unsigned)nCode, (unsigned long)nAttr,
                                  (unsigned long)adr, (unsigned long)len,
                                  (unsigned long)(pA != NULL ? pA->Adr : 0),
                                  (unsigned long)nE);
        }
        return nE;
    } catch (...) {
        AgdiLog::Write("AG_MemAtt", "EXCEPTION");
        return AG_NOACCESS;
    }
}

/* --------------------------------------------------------------------------
 * Breakpoints (B.3)
 *
 * Official contract (AGDI.H / official AGDI.CPP):
 *   AG_BpInfo  - `vp` is a GADR*; the nCode decides what is asked:
 *                AG_BPQUERY / AG_BPEXQUERY return the ATTRX_BREAK / ATTRX_BPDIS
 *                bits of that address; the enable/disable/kill/set forms are
 *                notifications that return 0; AG_BPDISALL / AG_BPKILLALL return
 *                the number of affected execution breakpoints.
 *   AG_BreakFunc - lifecycle notification of µVision's own breakpoint objects
 *                (nCode 1 = "will be linked", 2 = "will be unlinked",
 *                4 = "enabled state may have changed", 5 = "accept this
 *                breakpoint?" -> return pB or NULL to refuse). Codes 3/6/7/8
 *                are documented as "not sent" and are ignored.
 * Both are thin wrappers over AgdiBreakpoints; only EXECUTION breakpoints are
 * accepted (data watchpoints / conditional / trace stay unsupported, 30/31/96).
 * ------------------------------------------------------------------------ */

U32 __cdecl AG_BpInfo(U16 nCode, void *vp) {
    try {
        const GADR *pA = (const GADR *)vp;
        const uint32_t adr = (pA != NULL) ? (uint32_t)pA->Adr : 0u;
        U32 result = 0;

        if (!AgdiSession::Usable()) {
            /* official sample: "PlayDead" -> 0 (and µVision keeps the driver) */
            AgdiLog::WriteRateLimited("AG_BpInfo", "nCode=0x%02X adr=0x%08lX -> 0 "
                                      "(no session)", (unsigned)nCode,
                                      (unsigned long)adr);
            return 0;
        }

        switch (nCode) {
            case AG_BPQUERY:      /* query break attributes of that address */
            case AG_BPEXQUERY:    /* query incl. "executed" attribute */
                result = AgdiBreakpoints::AttributeBits(adr);
                break;

            case AG_BPENABLE:     /* notification: enable breakpoint */
                AgdiBreakpoints::NotifyEnable(adr);
                break;

            case AG_BPDISABLE:    /* notification: disable breakpoint */
                AgdiBreakpoints::NotifyDisable(adr);
                break;

            case AG_BPKILL:       /* notification: kill breakpoint */
                AgdiBreakpoints::NotifyKill(adr);
                break;

            case AG_BPSET:        /* notification: set breakpoint */
                AgdiBreakpoints::NotifySet(adr);
                break;

            case AG_BPDISALL:     /* notification: all breakpoints disabled */
                result = AgdiBreakpoints::DisableAll();
                break;

            case AG_BPKILLALL:    /* notification: all breakpoints killed */
                result = AgdiBreakpoints::KillAll();
                break;

            default:
                AgdiLog::Write("AG_BpInfo", "unhandled nCode=0x%02X adr=0x%08lX",
                               (unsigned)nCode, (unsigned long)adr);
                break;
        }
        AgdiLog::WriteRateLimited("AG_BpInfo", "tid=%lu nCode=0x%02X(%s) adr=0x%08lX "
                                  "-> %lu (backend=%u logical=%u temp=%u)",
                                  (unsigned long)GetCurrentThreadId(), (unsigned)nCode,
                                  BpInfoName(nCode), (unsigned long)adr,
                                  (unsigned long)result, AgdiBreakpoints::BackendCount(),
                                  AgdiBreakpoints::LogicalCount(),
                                  AgdiBreakpoints::TemporaryCount());
        return result;
    } catch (...) {
        AgdiLog::Write("AG_BpInfo", "EXCEPTION nCode=0x%04X", (unsigned)nCode);
        return 0;
    }
}

AG_BP *__cdecl AG_BreakFunc(U16 nCode, U16 n1, GADR *pA, AG_BP *pB) {
    try {
        const uint32_t adr = (pB != NULL) ? (uint32_t)pB->Adr
                                          : (pA != NULL ? (uint32_t)pA->Adr : 0u);
        const unsigned type = (pB != NULL) ? (unsigned)pB->type : 0xFFFFFFFFu;
        const bool enabled = (pB != NULL) && (pB->enabled != 0);

        if (!AgdiSession::Usable()) {
            AgdiLog::WriteRateLimited("AG_BreakFunc", "nCode=%u n1=%u adr=0x%08lX -> "
                                      "NULL (no session)", (unsigned)nCode,
                                      (unsigned)n1, (unsigned long)adr);
            return NULL;
        }

        switch (nCode) {
            case 1:   /* notification: 'pB' will be linked into the list */
                if (type != 0xFFFFFFFFu && type != AG_ABREAK) {
                    AgdiLog::Write("AG_BreakFunc", "nCode=1 unsupported breakpoint "
                                   "type=%u adr=0x%08lX (only AG_ABREAK)",
                                   type, (unsigned long)adr);
                    break;
                }
                AgdiBreakpoints::NotifyLink(adr, enabled, (uint32_t)n1);
                break;

            case 2:   /* notification: 'pB' will be unlinked from the list */
                AgdiBreakpoints::NotifyUnlink(adr, (uint32_t)n1);
                break;

            case 4:   /* notification: 'pB->enabled' may have changed */
                AgdiBreakpoints::NotifyEnabled(adr, (uint32_t)n1, enabled);
                break;

            case 5:   /* "Bp-accept" function: return pB to accept, NULL to refuse */
                if (pB != NULL && pB->type != AG_ABREAK) {
                    AgdiLog::Write("AG_BreakFunc", "nCode=5 REFUSED type=%u adr=0x%08lX "
                                   "(execution breakpoints only)", type,
                                   (unsigned long)adr);
                    return NULL;   // official "unsupported breakpoint" answer
                }
                break;

            case 3:   /* documented as "not sent to target" */
            case 6:
            case 7:
            case 8:
                break;

            default:
                AgdiLog::Write("AG_BreakFunc", "unhandled nCode=%u n1=%u adr=0x%08lX",
                               (unsigned)nCode, (unsigned)n1, (unsigned long)adr);
                break;
        }
        AgdiLog::Write("AG_BreakFunc", "tid=%lu nCode=%u n1=%u adr=0x%08lX type=%u "
                       "enabled=%d -> %s (backend=%u logical=%u temp=%u)",
                       (unsigned long)GetCurrentThreadId(), (unsigned)nCode,
                       (unsigned)n1, (unsigned long)adr, type, enabled ? 1 : 0,
                       pB != NULL ? "pB" : "NULL", AgdiBreakpoints::BackendCount(),
                       AgdiBreakpoints::LogicalCount(),
                       AgdiBreakpoints::TemporaryCount());
        return pB;
    } catch (...) {
        AgdiLog::Write("AG_BreakFunc", "EXCEPTION nCode=%u", (unsigned)nCode);
        return NULL;
    }
}

/* --------------------------------------------------------------------------
 * Execution control (B.2: instruction step; B.3: Run/Stop + temporary bp)
 *
 * Official semantics (AppNote 173 "Execute Program Code" + AGDI.CPP):
 *   * AG_GoStep runs in a µVision thread of its own and RETURNS when execution
 *     stops (blocking). The Stop toolbar button calls AG_GoStep(AG_STOPRUN)
 *     from ANOTHER thread while that call is still blocked.
 *   * AG_GOTILADR / AG_GOFORBRK run until a breakpoint (or user halt / fault);
 *     GOTILADR installs an internal temporary breakpoint at pA->Adr.
 *   * The return value is the official error code space (0 = ok; AG_STOPRUN
 *     answers the StopExec encoding 1 = stopped / 0 = still executing).
 *   * After execution µVision is told where the PC is: *pCURPC is refreshed
 *     (official AGDI.CPP: GetRegs(); *pCURPC = REGARM.cur[15]).
 * The execution wait is NOT an RPC timeout -- it ends only when the target
 * stops, the connection dies or the session is torn down (section 137).
 * ------------------------------------------------------------------------ */

U32 __cdecl AG_GoStep(U16 nCode, U32 nSteps, GADR *pA) {
    try {
        const bool vt = AgdiLog::Verbose();
        if (vt) {
            AgdiLog::WriteVerbose("AG_GoStep",
                                  "nCode=0x%02X(%s) nSteps=%lu adr=0x%08lX start",
                                  (unsigned)nCode, GoStepName(nCode),
                                  (unsigned long)nSteps,
                                  (unsigned long)(pA != NULL ? pA->Adr : 0));
        }
        const U32 nE = GoStepImpl(nCode, nSteps, pA);
        if (vt) {
            AgdiLog::WriteVerbose("AG_GoStep", "nCode=0x%02X(%s) -> %lu end",
                                  (unsigned)nCode, GoStepName(nCode),
                                  (unsigned long)nE);
        }
        return nE;
    } catch (...) {
        AgdiLog::Write("AG_GoStep", "EXCEPTION nCode=0x%04X", (unsigned)nCode);
        return AG_INVALOP;
    }
}

/* --------------------------------------------------------------------------
 * Serial window / trace history - unchanged B.1 answers
 * ------------------------------------------------------------------------ */

U32 __cdecl AG_Serial(U16 nCode, U32 nSerNo, U32 nMany, void *vp) {
    try {
        AgdiLog::WriteRateLimited("AG_Serial", "nCode=0x%02X nSerNo=%lu nMany=%lu -> 0",
                                  (unsigned)nCode, (unsigned long)nSerNo,
                                  (unsigned long)nMany);
        (void)nCode; (void)nSerNo; (void)nMany; (void)vp;
    } catch (...) {
        AgdiLog::Write("AG_Serial", "EXCEPTION");
    }
    return 0;
}

U32 __cdecl AG_HistFunc(U32 nCode, I32 indx, I32 dir, void *vp) {
    try {
        AgdiLog::WriteRateLimited("AG_HistFunc", "nCode=%lu indx=%ld dir=%ld -> 0 (no trace)",
                                  (unsigned long)nCode, (long)indx, (long)dir);
        (void)nCode; (void)indx; (void)dir; (void)vp;
    } catch (...) {
        AgdiLog::Write("AG_HistFunc", "EXCEPTION");
    }
    return 0;
}

}  // extern "C"