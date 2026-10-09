# Keil µVision AGDI driver — BlueBridge

`BlueBridgeAGDI.dll` makes **Keil µVision** treat BlueBridgeSimulator as a real
Cortex-M target debug driver:

```
µVision (UV4.exe, 32-bit)
    -> BlueBridgeAGDI.dll            (MSVC, Win32/x86, thin, no Qt / no core)
        -> Windows Named Pipe        (docs/debug_ipc_protocol.md v1.0)
            -> bluesim.exe --debug-pipe <name> --wait-debugger
                -> DebugIpcServer -> SimulatorDebugTarget / SimulatorFlashProgrammer
                    -> Simulator -> Unicorn Cortex-M4F -> STM32G431 + CT117E-M4
```

Two hard rules:

1. **The DLL is thin.** It never links `bluesim_core`, Qt or unicorn, and never
   passes STL/Qt/exceptions across the DLL boundary. The only boundary is the
   Named Pipe wire protocol.
2. **The ABI is not guessed.** Every prototype, struct and nCode comes from the
   official Keil AGDI material (`vendor/README.md`). The DLL matches the local
   `UV4.exe` bitness (x86) and registers one TDRV slot in `[ARM]` / `[ARMADS]`
   (Cortex-M `CPUDLL1` group).

Verified setup: µVision 5.43.1.0, DLL built with the VS 2022 Build Tools,
`/MT` static CRT, 12/12 official export names checked with dumpbin at build time.

## Layout

| Path | Purpose |
|---|---|
| `BlueBridgeAGDI/src/` | driver sources: DllMain, AgdiDriver / AgdiSession / AgdiConfig / AgdiLog, AgdiRegisters / AgdiMemory / AgdiProgram, IpcClient / IpcProtocol |
| `BlueBridgeAGDIProbe/src/` | MSVC x86 probe: exercises the pipe without Keil |
| `build_agdi.ps1` | builds the DLL (version resource, export/dependency checks) + the probe |
| `install.ps1` / `uninstall.ps1` | register / unregister the driver (in-place TOOLS.INI edit, idempotent) |
| `run_keil_b3_tests.ps1` | breakpoints / run-stop regression (9 scenarios) |
| `run_keil_b4_trace.ps1` | load-semantics trace tool (init-file experiments) |
| `run_keil_b4_download.ps1` | application-load suite (10 scenarios) |
| `run_keil_b4_source.ps1` | source-level suite: run-to-main, watch, locals, call stack, stepping (16 scenarios) |
| `run_keil_session_stress.ps1` | N automated µVision sessions (AutoStart or fixed pipe) |
| `run_installer_stress.ps1` | install / uninstall stress (20 rounds) |
| `run_cleanenv_e2e.ps1` | clean-environment end-to-end QA |
| `keil_test/` | `bb_test` fixture (exit_init.ini) + `bb_full` full self-test project |
| `vendor/` | official Keil AGDI SDK (git-ignored; see `vendor/README.md`) |
| `build/` | compiler output (git-ignored) |

## Build / install

```powershell
# toolchain: VS 2022 Build Tools, x86 tools (auto-detected via vswhere;
# override with -VsPath <root> when detection fails)
powershell -File tools\keil_agdi\build_agdi.ps1 -AgdiSdkDir tools\keil_agdi\vendor\agdi_sdk\SampTargN
powershell -File tools\keil_agdi\install.ps1        # needs the DLL first
powershell -File tools\keil_agdi\uninstall.ps1
```

`build_agdi.ps1` reads the bitness of the local `UV4.exe` (PE machine) and
builds for the matching platform (x86 here).

## Verification

```powershell
# probe: MSVC x86 <-> named pipe <-> MinGW64 bluesim.exe, no Keil involved
tools\keil_agdi\build\Release\BlueBridgeAGDIProbe.exe --rounds 20

# Keil lifecycle stress over a fixed pipe (agdi.ini is switched to
# AutoStart=0 + PipeName=... for the run and restored afterwards)
powershell -File tools\keil_agdi\run_keil_session_stress.ps1 -Rounds 50 `
    -PipeName BlueBridgeSimulator.Debug.KeilStress

# simulator-side regression
ctest --test-dir <simulator build dir> --output-on-failure
```

## Keil files are NOT committed

The official Keil AGDI header/template (`AGDI.H`, `AGDI.CPP`, `COMTYP.H`,
`SampTarg.*`) is copyrighted by Keil/Arm and is **not redistributed in this
repository**. `vendor/README.md` documents how to obtain it from Keil's own
server; `vendor/agdi_sdk/` and `build/` are git-ignored.