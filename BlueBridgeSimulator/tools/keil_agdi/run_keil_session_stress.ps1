# BlueBridge AGDI - Keil debug-session lifecycle stress (spec 7-2B sections 29 / 121)
#
# Runs `UV4 -d <project> -j0 -sg` N times. Each invocation starts a debug
# session (loads BlueBridgeAGDI.dll, runs exit_init.ini which contains EXIT for
# automated runs, then unloads the driver). The script then reports how many
# sessions connected / unloaded according to the driver's own log
# ("HELLO session=" lines from the IPC layer).
#
# Session modes:
#   default                     AutoStart=1 without PipeName: every round lets
#                               the driver launch its own simulator (which stays
#                               alive because of LeaveSimulatorRunning=1)
#   -PipeName / -SimulatorExe   one fixed-pipe simulator is started up front,
#                               agdi.ini is switched to AutoStart=0 +
#                               PipeName=<name> (backed up first, restored
#                               afterwards) and every round reuses it
#
# Usage:
#   powershell -File tools\keil_agdi\run_keil_session_stress.ps1 -Rounds 50
#   powershell -File tools\keil_agdi\run_keil_session_stress.ps1 -Rounds 50 `
#       -PipeName BlueBridgeSimulator.Debug.KeilStress
#
# MUST run outside the agent sandbox: µVision writes its per-user settings and
# the driver writes %LOCALAPPDATA%\BlueBridgeSimulator\logs\BlueBridgeAGDI.log.

[CmdletBinding()]
param(
    [int]$Rounds = 50,
    [int]$TimeoutSec = 30,
    [string]$ProjectPath = "",
    [string]$Uv4Path = "",
    [string]$SimulatorExe = "",
    [string]$PipeName = "",
    [switch]$KeepSimulator
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$simRoot   = Split-Path -Parent (Split-Path -Parent $scriptDir)   # BlueBridgeSimulator
$proj = if ($ProjectPath) { $ProjectPath } else { Join-Path $scriptDir 'keil_test\bb_test.uvprojx' }
$uv4  = if ($Uv4Path)     { $Uv4Path }     else { Join-Path $env:LOCALAPPDATA 'Keil_v5\UV4\UV4.exe' }

if (-not (Test-Path $proj)) { throw "project not found: $proj" }
if (-not (Test-Path $uv4))  { throw "UV4.exe not found: $uv4" }

$outDir  = Join-Path (Split-Path -Parent $proj) 'lifecycle'
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$agdiLog = Join-Path $env:LOCALAPPDATA 'BlueBridgeSimulator\logs\BlueBridgeAGDI.log'
$agdiIni = Join-Path $env:LOCALAPPDATA 'BlueBridgeSimulator\agdi.ini'

function Count-Matches([string]$Path, [string]$Pattern) {
    if (-not (Test-Path $Path)) { return 0 }
    return (Select-String -Path $Path -Pattern $Pattern -ErrorAction SilentlyContinue | Measure-Object).Count
}

function Test-PipeExists([string]$Name) {
    return @(Get-ChildItem '\\.\pipe\' -ErrorAction SilentlyContinue |
             Where-Object { $_.Name -eq $Name }).Count -gt 0
}

# stale µVision instances on THIS project hold the DLL and the single-instance
# mutex (B.4.2 spec 82/83: never kill the user's other Keil windows)
Get-CimInstance Win32_Process -Filter "Name='UV4.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -and $_.CommandLine -like "*$proj*" } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep -Milliseconds 500

# --------------------------------------------------------------------------
# fixed-pipe mode: start one simulator and point agdi.ini at it (backed up)
# --------------------------------------------------------------------------
$useFixedPipe = ($PipeName -ne '') -or ($SimulatorExe -ne '')
$simProc   = $null
$iniBackup = $null
$iniExisted = Test-Path $agdiIni

if ($useFixedPipe) {
    if (-not $SimulatorExe) {
        $candidates = @(
            (Join-Path $simRoot 'build\bluesim.exe'),
            (Join-Path $simRoot 'build\Release\bluesim.exe'),
            (Join-Path $simRoot 'release\bluesim.exe')
        )
        foreach ($c in $candidates) { if (Test-Path $c) { $SimulatorExe = $c; break } }
    }
    if (-not $SimulatorExe -or -not (Test-Path $SimulatorExe)) {
        throw "no simulator found; pass -SimulatorExe or build bluesim.exe first"
    }
    if (-not $PipeName) { $PipeName = 'BlueBridgeSimulator.Debug.KeilStress' }

    if ($iniExisted) {
        $iniBackup = "$agdiIni.keilstress.bak"
        Copy-Item -LiteralPath $agdiIni -Destination $iniBackup -Force
    }
    $lines = @()
    if ($iniExisted) { $lines = @(Get-Content -LiteralPath $agdiIni) }
    if (-not ($lines | Where-Object { $_ -match '^\s*\[BlueBridge\]' })) {
        $lines = @('[BlueBridge]') + $lines
    }
    $sawAuto = $false
    $sawPipe = $false
    $patched = foreach ($l in $lines) {
        if     ($l -match '^\s*AutoStart\s*=') { $sawAuto = $true; 'AutoStart=0' }
        elseif ($l -match '^\s*PipeName\s*=')  { $sawPipe = $true; "PipeName=$PipeName" }
        else                                   { $l }
    }
    if (-not $sawAuto) { $patched += 'AutoStart=0' }
    if (-not $sawPipe) { $patched += "PipeName=$PipeName" }
    [System.IO.File]::WriteAllLines($agdiIni, $patched, (New-Object System.Text.UTF8Encoding($false)))

    $simProc = Start-Process -FilePath $SimulatorExe `
        -ArgumentList @('--debug-pipe', $PipeName, '--wait-debugger') -PassThru
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline -and -not (Test-PipeExists $PipeName)) {
        Start-Sleep -Milliseconds 100
    }
    if (-not (Test-PipeExists $PipeName)) {
        throw "simulator never created pipe \\.\pipe\$PipeName"
    }
    Write-Host ("fixed pipe: {0} (simulator pid {1}: {2})" -f $PipeName, $simProc.Id, $SimulatorExe)
} else {
    Write-Host "mode: AutoStart=1 (each round launches its own simulator, LeaveSimulatorRunning=1)"
}

$s0 = Count-Matches $agdiLog 'HELLO session='
$u0 = Count-Matches $agdiLog 'DLL_PROCESS_DETACH'

$rows = @()
try {
    for ($i = 1; $i -le $Rounds; $i++) {
        $out = Join-Path $outDir ('run_{0:d3}.txt' -f $i)
        if (Test-Path $out) { Remove-Item $out -Force }

        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $pr = Start-Process -FilePath $uv4 -ArgumentList @('-d', $proj, '-j0', '-sg', ('-o' + $out)) -PassThru -WindowStyle Hidden
        $finished = $pr.WaitForExit($TimeoutSec * 1000)
        if (-not $finished) { $pr.Kill(); Start-Sleep -Milliseconds 500 }
        $sw.Stop()

        $exit = if ($finished) { $pr.ExitCode } else { 'TIMEOUT' }
        $rows += [pscustomobject]@{ Round = $i; Exit = $exit; Sec = [Math]::Round($sw.Elapsed.TotalSeconds, 1) }
        if (-not $finished) { Write-Host ("round {0}: TIMEOUT (killed)" -f $i) -ForegroundColor Red }
    }
} finally {
    # stop the simulator we started and restore the user's agdi.ini
    if ($simProc -and -not $KeepSimulator) {
        Stop-Process -Id $simProc.Id -Force -ErrorAction SilentlyContinue
    }
    if ($iniExisted) {
        if ($iniBackup) {
            Copy-Item -LiteralPath $iniBackup -Destination $agdiIni -Force
            Remove-Item -LiteralPath $iniBackup -Force
        }
    } else {
        Remove-Item -LiteralPath $agdiIni -Force -ErrorAction SilentlyContinue
    }
}

$s1 = Count-Matches $agdiLog 'HELLO session='
$u1 = Count-Matches $agdiLog 'DLL_PROCESS_DETACH'

Write-Host ""
$rows | Format-Table -AutoSize
Write-Host ("sessions connected    : {0} (delta {1})" -f $s1, ($s1 - $s0))
Write-Host ("driver unloads        : {0} (delta {1}; two per process: driver-list probe + exit)" -f $u1, ($u1 - $u0))
$bad = @($rows | Where-Object { $_.Exit -ne 0 }).Count
$connected = $s1 - $s0
if ($bad -eq 0 -and $connected -eq $Rounds) {
    Write-Host ("result: {0}/{0} rounds OK (exit 0), {0} IPC sessions, no timeout, no crash" -f $Rounds) -ForegroundColor Green
} else {
    Write-Host ("result: FAILED - {0} bad exit(s), {1} IPC session(s) for {2} rounds" -f $bad, $connected, $Rounds) -ForegroundColor Red
}
Write-Host ("per-round logs        : {0}" -f $outDir)