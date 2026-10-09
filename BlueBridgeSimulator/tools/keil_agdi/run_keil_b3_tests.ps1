# BlueBridge AGDI -- B.3 Keil run-control scenarios (stage 7-2B.3 sections 74-99,
# 125-131).
#
# Runs REAL µVision debug sessions against a preloaded virtual flash image and
# drives them through the official debugger command language (verified against
# the µVision 4 Command Window documentation: BS/BL/BD/BE/BK/G/T/P/EXIT). The
# command script is written into the project's
# initialization file (`keil_test/exit_init.ini`, referenced by tIfile in
# bb_test.uvoptx), so no UI automation is needed.
#
# Every scenario:
#   1. starts one simulator on a fixed pipe,
#   2. PRELOADS `keil_test/UVBuild/bb_test.hex` through the Debug IPC (B.3 test
#      fixture -- this is NOT the AGDI application download, which is B.4),
#   3. points agdi.ini at that pipe (AutoStart=0),
#   4. runs UV4 -d ... -j0 -sg with the scenario's command script,
#   5. snapshots the AGDI driver log delta as the evidence.
#
# Usage:
#   powershell -File tools\keil_agdi\run_keil_b3_tests.ps1
#   powershell -File tools\keil_agdi\run_keil_b3_tests.ps1 -Scenarios continue -Rounds 100
#
# MUST run outside the agent sandbox (µVision writes per-user settings, the
# driver writes %LOCALAPPDATA%\BlueBridgeSimulator\logs\BlueBridgeAGDI.log).

[CmdletBinding()]
param(
    [string[]]$Scenarios = @(),
    [int]$Rounds = 1,
    [int]$TimeoutSec = 60,
    [string]$PipeName = 'BlueBridgeSimulator.Debug.B3',
    [switch]$KeepSimulator,
    [switch]$NoPreload
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$simRoot   = Split-Path -Parent (Split-Path -Parent $scriptDir)   # BlueBridgeSimulator
$testDir   = Join-Path $scriptDir 'keil_test'
$proj      = Join-Path $testDir 'bb_test.uvprojx'
$hex       = Join-Path $testDir 'UVBuild\bb_test.hex'
$initFile  = Join-Path $testDir 'exit_init.ini'
$probe     = Join-Path $scriptDir 'build\Release\BlueBridgeAGDIProbe.exe'
$outDir    = Join-Path $scriptDir 'build\b3_keil'
$uv4       = Join-Path $env:LOCALAPPDATA 'Keil_v5\UV4\UV4.exe'
$agdiLog   = Join-Path $env:LOCALAPPDATA 'BlueBridgeSimulator\logs\BlueBridgeAGDI.log'
$agdiIni   = Join-Path $env:LOCALAPPDATA 'BlueBridgeSimulator\agdi.ini'

if (-not (Test-Path $proj))  { throw "project not found: $proj" }
if (-not (Test-Path $uv4))   { throw "UV4.exe not found: $uv4" }
if (-not (Test-Path $probe)) { throw "probe not found: $probe (run build_agdi.ps1)" }
if (-not $NoPreload -and -not (Test-Path $hex)) { throw "fixture hex not found: $hex (build the Keil project first)" }
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

# --------------------------------------------------------------------------
# scenario command scripts (official µVision debug commands only)
#
# Source-level breakpoints (`BS main`, `G,bar`, ...) need µVision's symbol
# table. With "Load Application at Startup" OFF (B.3 fixture mode, spec 120) the
# table stays empty and `BS main` fails with "*** error 34: undefined
# identifier". `LOAD %L NOCODE INCREMENTAL` loads the AXF symbols WITHOUT
# writing any code record and without a target reset (official LOAD options),
# after which µVision resolves source symbols to machine addresses itself
# (spec 32/59/91). Local fixture functions: add_one, foo, bar, main; `bar` and
# the main-loop BL are the repeatedly executed targets (main's entry runs once).
# --------------------------------------------------------------------------
$symLoad = 'LOAD %L NOCODE INCREMENTAL'
$scenarioScripts = [ordered]@{
    # run until a source breakpoint, then leave the session
    'run-bp'            = @($symLoad, 'BL', 'BS main', 'BL', 'G', 'EXIT')
    # F5 (continue) from a breakpoint twice (bar runs once per loop iteration)
    'continue'          = @($symLoad, 'BS bar', 'G', 'G', 'EXIT')
    # multiple source breakpoints, hit/continue each in execution order
    'multi-bp'          = @($symLoad, 'BS add_one', 'BS foo', 'BS bar', 'BS main',
                            'BL', 'G', 'G', 'G', 'G', 'EXIT')
    # disable the first breakpoint, run (must stop at the second), re-enable
    'disable-enable'    = @($symLoad, 'BS bar', 'BS main', 'BL', 'BD 0', 'BL',
                            'G', 'BE 0', 'G', 'EXIT')
    # kill the first breakpoint, run (must stop at the remaining one)
    'kill'              = @($symLoad, 'BS bar', 'BS main', 'BL', 'BK 0', 'BL',
                            'G', 'EXIT')
    # run to a source location without a user breakpoint (G,<symbol>: temp bp)
    'run-to-address'    = @($symLoad, 'G,bar', 'EXIT')
    # instruction step then run
    'step-then-run'     = @($symLoad, 'T', 'T', 'BS bar', 'G', 'EXIT')
    # clear all breakpoints through the kill-all path, then run + stop
    'kill-out-of-loop'  = @($symLoad, 'BS main', 'BK 0', 'G,bar', 'EXIT')
    # dedicated B.3.4 source-breakpoint scenario (spec 90-95): BL shows the
    # resolved address, G stops on the C-line breakpoint
    'source-bp'         = @($symLoad, 'BL', 'BS bar', 'BL', 'G', 'EXIT')
}

if ($Scenarios.Count -eq 0) { $Scenarios = @($scenarioScripts.Keys) }
foreach ($s in $Scenarios) {
    if (-not $scenarioScripts.Contains($s)) {
        throw "unknown scenario '$s' (known: $($scenarioScripts.Keys -join ', '))"
    }
}

# --------------------------------------------------------------------------
# environment setup: close stale UV4 instances running THIS project only
# (B.4.2 spec 82/83: never kill the user's other Keil windows), start one
# simulator, patch agdi.ini
# --------------------------------------------------------------------------
Get-CimInstance Win32_Process -Filter "Name='UV4.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -and $_.CommandLine -like "*$proj*" } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep -Milliseconds 400

$simExe = @(
    (Join-Path $simRoot 'build\bluesim.exe'),
    (Join-Path $simRoot 'release\bluesim.exe')
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $simExe) { throw "no simulator found (build bluesim.exe first)" }

$iniExisted = Test-Path $agdiIni
$iniBackup = "$agdiIni.b3.bak"
if ($iniExisted) { Copy-Item -LiteralPath $agdiIni -Destination $iniBackup -Force }
$lines = @()
if ($iniExisted) { $lines = @(Get-Content -LiteralPath $agdiIni) }
if (-not ($lines | Where-Object { $_ -match '^\s*\[BlueBridge\]' })) {
    $lines = @('[BlueBridge]') + $lines
}
$sawAuto = $false; $sawPipe = $false
$patched = foreach ($l in $lines) {
    if     ($l -match '^\s*AutoStart\s*=') { $sawAuto = $true; 'AutoStart=0' }
    elseif ($l -match '^\s*PipeName\s*=')  { $sawPipe = $true; "PipeName=$PipeName" }
    else                                   { $l }
}
if (-not $sawAuto) { $patched += 'AutoStart=0' }
if (-not $sawPipe) { $patched += "PipeName=$PipeName" }
[System.IO.File]::WriteAllLines($agdiIni, $patched, (New-Object System.Text.UTF8Encoding($false)))

$simProc = Start-Process -FilePath $simExe -ArgumentList @('--debug-pipe', $PipeName, '--wait-debugger') -PassThru
$deadline = (Get-Date).AddSeconds(15)
while ((Get-Date) -lt $deadline -and
       -not @(Get-ChildItem '\\.\pipe\' -ErrorAction SilentlyContinue | Where-Object { $_.Name -eq $PipeName }).Count) {
    Start-Sleep -Milliseconds 100
}
Write-Host ("simulator pid {0}, pipe {1}" -f $simProc.Id, $PipeName)

$initBackup = "$initFile.b3.bak"
Copy-Item -LiteralPath $initFile -Destination $initBackup -Force

function Count-Matches([string]$Path, [string]$Pattern) {
    if (-not (Test-Path $Path)) { return 0 }
    return (Select-String -Path $Path -Pattern $Pattern -ErrorAction SilentlyContinue | Measure-Object).Count
}

$rows = @()
try {
    foreach ($s in $Scenarios) {
        for ($r = 1; $r -le $Rounds; $r++) {
            # 1. preload the fixture image (fresh virtual flash per scenario)
            if (-not $NoPreload) {
                & $probe --pipe $PipeName --hex $hex --preload | Out-Null
                if ($LASTEXITCODE -ne 0) { throw "preload failed for scenario $s" }
            }

            # 2. write the scenario's debugger command script
            $scriptText = ($scenarioScripts[$s] -join "`r`n") + "`r`n"
            [System.IO.File]::WriteAllText($initFile, $scriptText,
                                           (New-Object System.Text.ASCIIEncoding))

            # 3. run the session, capture the driver log delta as evidence
            $logLen = 0
            if (Test-Path $agdiLog) { $logLen = (Get-Item $agdiLog).Length }
            $out = Join-Path $outDir ('{0}_{1:d3}_uv4.txt' -f $s, $r)
            if (Test-Path $out) { Remove-Item $out -Force }

            $sw = [System.Diagnostics.Stopwatch]::StartNew()
            $pr = Start-Process -FilePath $uv4 -ArgumentList @('-d', $proj, '-j0', '-sg', ('-o' + $out)) -PassThru -WindowStyle Hidden
            $finished = $pr.WaitForExit($TimeoutSec * 1000)
            if (-not $finished) { $pr.Kill(); Start-Sleep -Milliseconds 500 }
            $sw.Stop()

            $exit = if ($finished) { $pr.ExitCode } else { 'TIMEOUT' }
            $evidence = Join-Path $outDir ('{0}_{1:d3}_agdi.log' -f $s, $r)
            if (Test-Path $agdiLog) {
                $fs = [System.IO.File]::Open($agdiLog, 'Open', 'Read', 'ReadWrite')
                $fs.Seek($logLen, 'Begin') | Out-Null
                $sr = New-Object System.IO.StreamReader($fs)
                $delta = $sr.ReadToEnd()
                $sr.Close(); $fs.Close()
                Set-Content -LiteralPath $evidence -Value $delta -Encoding UTF8
            }

            $rows += [pscustomobject]@{ Scenario = $s; Round = $r; Exit = $exit
                                        Sec = [Math]::Round($sw.Elapsed.TotalSeconds, 1) }
            Write-Host ("{0} #{1}: exit={2} ({3}s)" -f $s, $r, $exit, [Math]::Round($sw.Elapsed.TotalSeconds, 1))
        }
    }
} finally {
    Copy-Item -LiteralPath $initBackup -Destination $initFile -Force
    Remove-Item -LiteralPath $initBackup -Force -ErrorAction SilentlyContinue
    if ($simProc -and -not $KeepSimulator) { Stop-Process -Id $simProc.Id -Force -ErrorAction SilentlyContinue }
    if ($iniExisted) {
        Copy-Item -LiteralPath $iniBackup -Destination $agdiIni -Force
        Remove-Item -LiteralPath $iniBackup -Force -ErrorAction SilentlyContinue
    } else {
        Remove-Item -LiteralPath $agdiIni -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ""
$rows | Format-Table -AutoSize
$bad = @($rows | Where-Object { $_.Exit -ne 0 }).Count
Write-Host ("result: {0}/{1} sessions exit=0" -f ($rows.Count - $bad), $rows.Count) -ForegroundColor $(if ($bad -eq 0) { 'Green' } else { 'Red' })
Write-Host ("evidence: {0}" -f $outDir)