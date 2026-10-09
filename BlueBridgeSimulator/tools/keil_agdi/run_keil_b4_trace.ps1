# BlueBridge AGDI -- B.4.1 application-load trace (stage 7-2B.4, checkpoint B.4.1)
#
# Purpose: measure what the REAL local µVision 5.43 does for
# "Load Application at Startup" (tLdApp=1) -- which AGDI exports it calls, in
# which order, with which nCodes/addresses -- instead of guessing from the
# 2003-era AppNote 173 sample. The driver is built with the LoadTrace facility
# (`LoadTrace=1` in agdi.ini): AG_MemAcc / AG_MemAtt / AG_GoStep / AG_AllReg /
# AG_RegAcc are recorded verbatim as `[TRACE]` lines (never rate limited),
# while the flash nCodes (AG_F_*) deliberately still answer AG_INVALOP
# (checkpoint B.4.1 rule: observe, do not fake success).
#
# Scenarios (each one: fresh simulator -> fresh 0xFF flash -> real session):
#   loadapp             tLdApp=1 tGomain=0  init "EXIT"
#   loadapp-runmain     tLdApp=1 tGomain=1  init "EXIT"
#   loadapp-runmain-hold tLdApp=1 tGomain=1  init empty; UV4 is killed after
#                        TimeoutSec so the post-load steady state is captured
#
# Every temporary project-file change (bb_test.uvoptx: tLdApp/tGomain/tIfile,
# exit_init.ini, agdi.ini) is made through backup + try/finally restore
# (spec sections 11 / 105): a failed run must never leave the fixture patched.
#
# MUST run outside the agent sandbox (µVision writes per-user settings; the
# driver writes %LOCALAPPDATA%\BlueBridgeSimulator\logs\BlueBridgeAGDI.log).
#
# Usage:
#   powershell -File tools\keil_agdi\run_keil_b4_trace.ps1
#   powershell -File tools\keil_agdi\run_keil_b4_trace.ps1 -Scenarios loadapp

[CmdletBinding()]
param(
    [string[]]$Scenarios = @(),
    [int]$TimeoutSec = 40,
    [string]$PipeName = 'BlueBridgeSimulator.Debug.B4Trace',
    [switch]$KeepSimulator
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$simRoot   = Split-Path -Parent (Split-Path -Parent $scriptDir)   # BlueBridgeSimulator
$testDir   = Join-Path $scriptDir 'keil_test'
$proj      = Join-Path $testDir 'bb_test.uvprojx'
$uvoptx    = Join-Path $testDir 'bb_test.uvoptx'
$initFile  = Join-Path $testDir 'exit_init.ini'
$outDir    = Join-Path $scriptDir 'build\b4_keil'
$uv4       = Join-Path $env:LOCALAPPDATA 'Keil_v5\UV4\UV4.exe'
$agdiLog   = Join-Path $env:LOCALAPPDATA 'BlueBridgeSimulator\logs\BlueBridgeAGDI.log'
$agdiIni   = Join-Path $env:LOCALAPPDATA 'BlueBridgeSimulator\agdi.ini'

if (-not (Test-Path $proj))   { throw "project not found: $proj" }
if (-not (Test-Path $uvoptx)) { throw "uvoptx not found: $uvoptx" }
if (-not (Test-Path $uv4))    { throw "UV4.exe not found: $uv4" }
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

# scenario table: LdApp / Gomain / init-file content
$scenarioDefs = [ordered]@{
    'loadapp'              = @{ LdApp = $true;  Gomain = $false; Init = @('EXIT') }
    'loadapp-runmain'      = @{ LdApp = $true;  Gomain = $true;  Init = @('EXIT') }
    'loadapp-runmain-hold' = @{ LdApp = $true;  Gomain = $true;  Init = @() }
    # Flash -> Download investigation (B.4.1 Q10 / B.4.4 section 70): send the
    # command-window candidates through the init file and log what µVision
    # answers (LOG writes the Command window output to a file). LdApp stays OFF
    # so the toolbar path is isolated from the startup load.
    'flashdownload-cmd'    = @{ LdApp = $false; Gomain = $false;
                                Init = @('LOG > {OUTDIR}\flashdownload_cmdlog.txt',
                                         'FLASH DOWNLOAD',
                                         'EXIT') }
    # LOG syntax check on its own (a command error aborts the rest of the
    # init file, so `LOG` and the candidate command must be verified separately)
    'logtest'              = @{ LdApp = $false; Gomain = $false;
                                Init = @('LOG > {OUTDIR}\logtest.txt',
                                         'EXIT') }
    # B.3 symbol-only workaround: which LOADPARMS flags does µVision announce
    # for `LOAD %L NOCODE INCREMENTAL`? (must show noCode=1, file=bb_test.axf)
    'nocode-symbols'       = @{ LdApp = $false; Gomain = $false;
                                Init = @('LOAD %L NOCODE INCREMENTAL',
                                         'EXIT') }
}
if ($Scenarios.Count -eq 0) { $Scenarios = @($scenarioDefs.Keys) }
foreach ($s in $Scenarios) {
    if (-not $scenarioDefs.Contains($s)) {
        throw "unknown scenario '$s' (known: $($scenarioDefs.Keys -join ', '))"
    }
}

$simExe = @(
    (Join-Path $simRoot 'build\bluesim.exe'),
    (Join-Path $simRoot 'release\bluesim.exe')
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $simExe) { throw "no simulator found (build bluesim.exe first)" }

# --------------------------------------------------------------------------
# uvoptx patch helpers (tLdApp / tGomain / tIfile)
# --------------------------------------------------------------------------
function Set-UvoptxOptions([string]$Path, [bool]$LdApp, [bool]$Gomain, [string]$Ifile) {
    $t = [System.IO.File]::ReadAllText($Path)
    $ld = if ($LdApp) { '1' } else { '0' }
    $gm = if ($Gomain) { '1' } else { '0' }
    $t = [regex]::Replace($t, '<tLdApp>[01]</tLdApp>', ('<tLdApp>' + $ld + '</tLdApp>'))
    $t = [regex]::Replace($t, '<tGomain>[01]</tGomain>', ('<tGomain>' + $gm + '</tGomain>'))
    $t = [regex]::Replace($t, '<tIfile>[^<]*</tIfile>', ('<tIfile>' + $Ifile + '</tIfile>'))
    [System.IO.File]::WriteAllText($Path, $t, (New-Object System.Text.UTF8Encoding($false)))
}

# --------------------------------------------------------------------------
# environment setup: close only stale instances of THIS project / THIS pipe
# (spec sections 82/83: a global UV4/bluesim kill would hit the user's own
# windows and simulators), then patch agdi.ini
# --------------------------------------------------------------------------
function Stop-TestUv4 {
    Get-CimInstance Win32_Process -Filter "Name='UV4.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -and $_.CommandLine -like "*$proj*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

function Stop-TestSim([string]$Pipe) {
    Get-CimInstance Win32_Process -Filter "Name='bluesim.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -and $_.CommandLine -like "*$Pipe*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

Stop-TestUv4
Stop-TestSim $PipeName
Start-Sleep -Milliseconds 500

$iniExisted = Test-Path $agdiIni
$iniBackup = "$agdiIni.b4t.bak"
if ($iniExisted) { Copy-Item -LiteralPath $agdiIni -Destination $iniBackup -Force }
$lines = @()
if ($iniExisted) { $lines = @(Get-Content -LiteralPath $agdiIni) }
if (-not ($lines | Where-Object { $_ -match '^\s*\[BlueBridge\]' })) {
    $lines = @('[BlueBridge]') + $lines
}
$sawAuto = $false; $sawPipe = $false; $sawTrace = $false; $sawLoad = $false
$patched = foreach ($l in $lines) {
    if     ($l -match '^\s*AutoStart\s*=')  { $sawAuto = $true; 'AutoStart=0' }
    elseif ($l -match '^\s*PipeName\s*=')   { $sawPipe = $true; "PipeName=$PipeName" }
    elseif ($l -match '^\s*Trace\s*=')      { $sawTrace = $true; 'Trace=1' }
    elseif ($l -match '^\s*LoadTrace\s*=')  { $sawLoad = $true; 'LoadTrace=1' }
    else                                    { $l }
}
if (-not $sawAuto)  { $patched += 'AutoStart=0' }
if (-not $sawPipe)  { $patched += "PipeName=$PipeName" }
if (-not $sawTrace) { $patched += 'Trace=1' }
if (-not $sawLoad)  { $patched += 'LoadTrace=1' }
[System.IO.File]::WriteAllLines($agdiIni, $patched, (New-Object System.Text.UTF8Encoding($false)))

$uvoptxBackup = "$uvoptx.b4t.bak"
$initBackup   = "$initFile.b4t.bak"
Copy-Item -LiteralPath $uvoptx -Destination $uvoptxBackup -Force
Copy-Item -LiteralPath $initFile -Destination $initBackup -Force

function Count-Marker([string]$Path, [string]$Pattern) {
    if (-not (Test-Path $Path)) { return 0 }
    return (Select-String -Path $Path -Pattern $Pattern -SimpleMatch -ErrorAction SilentlyContinue |
            Measure-Object).Count
}

$rows = @()
$simProc = $null
try {
    foreach ($s in $Scenarios) {
        $def = $scenarioDefs[$s]
        Write-Host ("--- scenario {0} (tLdApp={1} tGomain={2})" -f $s, $def.LdApp, $def.Gomain) -ForegroundColor Cyan

        # fresh simulator -> fresh 0xFF flash; the application load under test
        # must be the only thing that writes it (spec sections 12 / 37)
        if ($simProc) { Stop-Process -Id $simProc.Id -Force -ErrorAction SilentlyContinue; $simProc = $null }
        Start-Sleep -Milliseconds 300
        $simProc = Start-Process -FilePath $simExe -ArgumentList @('--debug-pipe', $PipeName, '--wait-debugger') -PassThru
        $deadline = (Get-Date).AddSeconds(15)
        while ((Get-Date) -lt $deadline -and
               -not @(Get-ChildItem '\\.\pipe\' -ErrorAction SilentlyContinue |
                      Where-Object { $_.Name -eq $PipeName }).Count) {
            Start-Sleep -Milliseconds 100
        }

        # project + init file state for this scenario
        Set-UvoptxOptions $uvoptx $def.LdApp $def.Gomain '.\exit_init.ini'
        $initText = (($def.Init | ForEach-Object { $_.Replace('{OUTDIR}', $outDir) }) -join "`r`n")
        if ($initText.Length -gt 0) { $initText += "`r`n" }
        [System.IO.File]::WriteAllText($initFile, $initText, (New-Object System.Text.ASCIIEncoding))

        $logLen = 0
        if (Test-Path $agdiLog) { $logLen = (Get-Item $agdiLog).Length }
        $out = Join-Path $outDir ('{0}_uv4.txt' -f $s)
        if (Test-Path $out) { Remove-Item $out -Force }

        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $pr = Start-Process -FilePath $uv4 -ArgumentList @('-d', $proj, '-j0', '-sg', ('-o' + $out)) -PassThru -WindowStyle Hidden
        $finished = $pr.WaitForExit($TimeoutSec * 1000)
        if (-not $finished) { $pr.Kill(); Start-Sleep -Milliseconds 800 }
        $sw.Stop()
        $exit = if ($finished) { $pr.ExitCode } else { 'TIMEOUT' }

        # driver log delta = the raw call sequence evidence
        $evidence = Join-Path $outDir ('{0}_agdi.log' -f $s)
        if (Test-Path $agdiLog) {
            $fs = [System.IO.File]::Open($agdiLog, 'Open', 'Read', 'ReadWrite')
            $fs.Seek($logLen, 'Begin') | Out-Null
            $sr = New-Object System.IO.StreamReader($fs)
            $delta = $sr.ReadToEnd()
            $sr.Close(); $fs.Close()
            Set-Content -LiteralPath $evidence -Value $delta -Encoding UTF8
        }

        $markers = [ordered]@{
            InitFlashLoad = Count-Marker $evidence 'AG_INITFLASHLOAD'
            StartFlashLoad = Count-Marker $evidence 'AG_STARTFLASHLOAD'
            InitStartLoad = Count-Marker $evidence 'AG_INITSTARTLOAD'
            InitEndLoad = Count-Marker $evidence 'AG_INITENDLOAD'
            F_Erase = Count-Marker $evidence 'AG_F_ERASE'
            F_Write = Count-Marker $evidence 'AG_F_WRITE'
            F_Verify = Count-Marker $evidence 'AG_F_VERIFY'
            F_Run = Count-Marker $evidence 'AG_F_RUN'
            WROPC = Count-Marker $evidence 'AG_WROPC'
            GoStep = Count-Marker $evidence 'AG_GoStep'
            TraceLines = Count-Marker $evidence '[TRACE]'
        }
        $rows += [pscustomobject]@{
            Scenario = $s; Exit = $exit; Sec = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
            InitFlashLoad = $markers.InitFlashLoad; StartFlashLoad = $markers.StartFlashLoad
            StartLoad = $markers.InitStartLoad; EndLoad = $markers.InitEndLoad
            F_Erase = $markers.F_Erase; F_Write = $markers.F_Write
            F_Verify = $markers.F_Verify; F_Run = $markers.F_Run
            WROPC = $markers.WROPC; GoStep = $markers.GoStep
            Trace = $markers.TraceLines
        }
        Write-Host ("    exit={0} ({1}s) traces={2} F_erase={3} F_write={4} WROPC={5} GoStep={6}" -f `
                    $exit, [Math]::Round($sw.Elapsed.TotalSeconds, 1), $markers.TraceLines,
                    $markers.F_Erase, $markers.F_Write, $markers.WROPC, $markers.GoStep)
    }
} finally {
    Copy-Item -LiteralPath $uvoptxBackup -Destination $uvoptx -Force
    Copy-Item -LiteralPath $initBackup -Destination $initFile -Force
    Remove-Item -LiteralPath $uvoptxBackup -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $initBackup -Force -ErrorAction SilentlyContinue
    if ($simProc -and -not $KeepSimulator) { Stop-Process -Id $simProc.Id -Force -ErrorAction SilentlyContinue }
    # always leave OUR Keil windows closed between automated runs (spec 82/83:
    # never touch UV4 instances that are not running this project)
    Stop-TestUv4
    Stop-TestSim $PipeName
    if ($iniExisted) {
        Copy-Item -LiteralPath $iniBackup -Destination $agdiIni -Force
        Remove-Item -LiteralPath $iniBackup -Force -ErrorAction SilentlyContinue
    } else {
        Remove-Item -LiteralPath $agdiIni -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ""
$rows | Format-Table -AutoSize
Write-Host ("evidence: {0}" -f $outDir)