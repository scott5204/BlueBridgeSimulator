# BlueBridge AGDI -- B.4.3 source-level debug acceptance
# (stage 7-2B.4 checkpoint B.4.3, spec sections 9 / 10-13 / 73-80).
#
# Drives REAL µVision 5.43 sessions in the FINAL user flow:
#
#   Build -> Start Debug Session -> Load Application at Startup (real
#   AG_INITSTARTLOAD -> AG_WROPC -> PROGRAM_* download) -> reset ->
#   Run to main() -> source breakpoints / watch / locals / call stack /
#   step into / over / out
#
# Rules kept from the spec:
#   * no probe preload, no GUI HEX load, no `LOAD %L NOCODE INCREMENTAL` in the
#     normal flow (the B.3 diagnostic keeps NOCODE for itself);
#   * the driver never parses AXF/ELF/DWARF/MAP -- the MAP file is used by THIS
#     script only, as a test oracle for "did µVision resolve the symbol";
#   * GUI-only facts (Watch/Locals/Call Stack window text, yellow arrow) are
#     marked MANUAL; the script proves the underlying
#     machine facts (PC/SP/LR/register+memory values, symbol addresses);
#   * every temporary file change (uvoptx / init file / agdi.ini / main.c)
#     happens under backup + try/finally restore;
#   * process management only touches instances of THIS project / THIS pipe.
#
# Usage (one scenario per call; no argument = all):
#   powershell -File tools\keil_agdi\run_keil_b4_source.ps1
#   powershell -File tools\keil_agdi\run_keil_b4_source.ps1 -Scenarios run-main
#
# MUST run outside the agent sandbox (µVision writes per-user settings, the
# driver writes %LOCALAPPDATA%\BlueBridgeSimulator\logs\BlueBridgeAGDI.log).

[CmdletBinding()]
param(
    [string[]]$Scenarios = @(),
    [int]$TimeoutSec = 60,
    [string]$PipeName = 'BlueBridgeSimulator.Debug.B4SRC',
    [int]$Rounds = 0,          # stress scenarios: override their round count
    [switch]$KeepSimulator
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$simRoot   = Split-Path -Parent (Split-Path -Parent $scriptDir)   # BlueBridgeSimulator
$testDir   = Join-Path $scriptDir 'keil_test'
$proj      = Join-Path $testDir 'bb_test.uvprojx'
$uvoptx    = Join-Path $testDir 'bb_test.uvoptx'
$uvprojx   = Join-Path $testDir 'bb_test.uvprojx'
$initFile  = Join-Path $testDir 'exit_init.ini'
$mainC     = Join-Path $testDir 'main.c'
$mapFile   = Join-Path $testDir 'bb_test.map'
$outDir    = Join-Path $scriptDir 'build\b4_src'
$probe     = Join-Path $scriptDir 'build\Release\BlueBridgeAGDIProbe.exe'
$uv4       = Join-Path $env:LOCALAPPDATA 'Keil_v5\UV4\UV4.exe'
$agdiLog   = Join-Path $env:LOCALAPPDATA 'BlueBridgeSimulator\logs\BlueBridgeAGDI.log'
$agdiIni   = Join-Path $env:LOCALAPPDATA 'BlueBridgeSimulator\agdi.ini'

foreach ($f in @($proj, $uvoptx, $mainC)) {
    if (-not (Test-Path $f)) { throw "missing fixture: $f" }
}
if (-not (Test-Path $uv4))   { throw "UV4.exe not found: $uv4" }
if (-not (Test-Path $probe)) { throw "probe not found (run build_agdi.ps1): $probe" }
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$simExe = @(
    (Join-Path $simRoot 'build\bluesim.exe'),
    (Join-Path $simRoot 'release\bluesim.exe')
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $simExe) { throw "no simulator found (build bluesim.exe first)" }

$allScenarios = @('run-main', 'run-main-ab', 'source-break', 'watch-global', 'locals',
                  'callstack', 'trace-steps', 'run-main-stress', 'step-walk', 'step-into',
                  'step-over', 'step-out', 'temp-bp-refcount', 'step-combined',
                  'step-runtime', 'lifecycle-stress')
if ($Scenarios.Count -eq 0) { $Scenarios = $allScenarios }
foreach ($s in $Scenarios) {
    if ($allScenarios -notcontains $s) {
        throw "unknown scenario '$s' (known: $($allScenarios -join ', '))"
    }
}

# ==========================================================================
# helpers
# ==========================================================================
$script:checks = @()
function Check([string]$name, [bool]$cond, [string]$detail = '') {
    $script:checks += [pscustomobject]@{ Name = $name; Ok = $cond; Detail = $detail }
    $tag = if ($cond) { '[ok]  ' } else { '[FAIL]' }
    $suffix = ''
    if ($detail) { $suffix = "  ($detail)" }
    Write-Host ("    {0} {1}{2}" -f $tag, $name, $suffix)
}

function Stop-TestUv4 {
    # only UV4 instances running THIS project -- never the user's other windows
    Get-CimInstance Win32_Process -Filter "Name='UV4.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -and $_.CommandLine -like "*$proj*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

function Stop-TestSim([string]$Pipe) {
    # only simulators attached to OUR pipe name
    Get-CimInstance Win32_Process -Filter "Name='bluesim.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -and $_.CommandLine -like "*$Pipe*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

function Start-TestSim([string]$Pipe) {
    Stop-TestSim $Pipe
    Start-Sleep -Milliseconds 400
    $p = Start-Process -FilePath $simExe -ArgumentList @('--debug-pipe', $Pipe, '--wait-debugger') -PassThru
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline -and
           -not @(Get-ChildItem '\\.\pipe\' -ErrorAction SilentlyContinue |
                  Where-Object { $_.Name -eq $Pipe }).Count) {
        Start-Sleep -Milliseconds 100
    }
    return $p
}

function Set-UvoptxOptions([string]$Path, [bool]$LdApp, [bool]$Gomain, [string]$Ifile) {
    $t = [System.IO.File]::ReadAllText($Path)
    $ld = if ($LdApp) { '1' } else { '0' }
    $gm = if ($Gomain) { '1' } else { '0' }
    $t = [regex]::Replace($t, '<tLdApp>[01]</tLdApp>', ('<tLdApp>' + $ld + '</tLdApp>'))
    $t = [regex]::Replace($t, '<tGomain>[01]</tGomain>', ('<tGomain>' + $gm + '</tGomain>'))
    $t = [regex]::Replace($t, '<tIfile>[^<]*</tIfile>', ('<tIfile>' + $Ifile + '</tIfile>'))
    [System.IO.File]::WriteAllText($Path, $t, (New-Object System.Text.UTF8Encoding($false)))
}

function Invoke-KeilBuild([string]$tag, [string]$OutDir) {
    $log = Join-Path $OutDir ("build_" + $tag + ".txt")
    $p = Start-Process -FilePath $uv4 -ArgumentList @('-b', $proj, '-j0', ('-o' + $log)) `
                       -PassThru -Wait -WindowStyle Hidden
    $text = if (Test-Path $log) { Get-Content -LiteralPath $log -Raw } else { '' }
    $errors = ([regex]::Matches($text, '(\d+) Error\(s\)') | ForEach-Object { [int]$_.Groups[1].Value } |
               Measure-Object -Maximum).Maximum
    if ($null -eq $errors) { $errors = 1 }
    return [pscustomobject]@{ Exit = $p.ExitCode; Errors = $errors; Log = $text }
}

function Get-MapSymbol([string]$MapPath, [string]$Name) {
    $t = [System.IO.File]::ReadAllText($MapPath)
    $m = [regex]::Match($t, ('(?m)^\s+' + [regex]::Escape($Name) + '\s+0x([0-9A-Fa-f]+)\s'))
    if (-not $m.Success) { return $null }
    return [Convert]::ToUInt32($m.Groups[1].Value, 16)
}

function Read-Mem32([string]$Pipe, [uint32]$Addr) {
    $line = & $probe --pipe $Pipe --read32 ('0x{0:X8}' -f $Addr) 2>&1 |
            Where-Object { $_ -like 'read32:*' } | Select-Object -First 1
    if (-not $line -or $line -notmatch 'value=0x([0-9A-Fa-f]+)') {
        throw "read32 failed at 0x$('{0:X8}' -f $Addr): $line"
    }
    return [Convert]::ToUInt32($Matches[1], 16)
}

function Read-MemBytes([string]$Pipe, [uint32]$Addr, [int]$Len) {
    $out = New-Object System.Collections.Generic.List[byte]
    $done = 0
    while ($done -lt $Len) {
        $chunk = [Math]::Min(0x10000, $Len - $done)
        $line = & $probe --pipe $Pipe --read ('0x{0:X8}' -f ($Addr + $done)) $chunk 2>&1 |
                Where-Object { $_ -like 'read:*' } | Select-Object -First 1
        if (-not $line -or $line -notmatch 'data=([0-9a-f]*)') {
            throw "read failed at 0x$('{0:X8}' -f ($Addr + $done)): $line"
        }
        $hex = $Matches[1]
        for ($i = 0; $i -lt $hex.Length; $i += 2) {
            $out.Add([Convert]::ToByte($hex.Substring($i, 2), 16))
        }
        $done += $chunk
    }
    return ,$out.ToArray()
}

function Read-Regs([string]$Pipe) {
    $line = & $probe --pipe $Pipe --regs 2>&1 |
            Where-Object { $_ -like 'regs:*' } | Select-Object -First 1
    if (-not $line) { throw "no regs output" }
    $h = @{}
    foreach ($m in [regex]::Matches($line, '([a-z0-9]+)=0x([0-9A-Fa-f]+)')) {
        $h[$m.Groups[1].Value] = [Convert]::ToUInt32($m.Groups[2].Value, 16)
    }
    return $h
}

function Read-State([string]$Pipe) {
    $line = & $probe --pipe $Pipe --state 2>&1 |
            Where-Object { $_ -like 'state:*' } | Select-Object -First 1
    if (-not $line -or $line -notmatch 'state=(\d+) stopReason=(\d+) pc=0x([0-9A-Fa-f]+)') {
        throw "no state output: $line"
    }
    return [pscustomobject]@{
        State = [int]$Matches[1]; StopReason = [int]$Matches[2]
        Pc = [Convert]::ToUInt32($Matches[3], 16)
    }
}

function Read-LogDelta([string]$Path, [long]$FromOffset) {
    if (-not (Test-Path $Path)) { return '' }
    $size = (Get-Item $Path).Length
    $start = $FromOffset
    if ($size -lt $FromOffset) { $start = 0 }   # 8 MiB rotation happened
    $fs = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    try {
        $fs.Seek($start, 'Begin') | Out-Null
        $sr = New-Object System.IO.StreamReader($fs)
        $text = $sr.ReadToEnd()
        $sr.Close()
        return $text
    } finally { $fs.Close() }
}

function Count-Marker([string]$Path, [string]$Pattern) {
    if (-not (Test-Path $Path)) { return 0 }
    return (Select-String -Path $Path -Pattern $Pattern -SimpleMatch -ErrorAction SilentlyContinue |
            Measure-Object).Count
}

function Count-Regex([string]$Path, [string]$Pattern) {
    if (-not (Test-Path $Path)) { return 0 }
    return (Select-String -Path $Path -Pattern $Pattern -ErrorAction SilentlyContinue |
            Measure-Object).Count
}

# last regex match of a group from a log file ('' when absent)
function Last-Match([string]$Path, [string]$Pattern, [int]$Group = 1) {
    if (-not (Test-Path $Path)) { return '' }
    $ms = @(Select-String -Path $Path -Pattern $Pattern -ErrorAction SilentlyContinue)
    if ($ms.Count -eq 0) { return '' }
    $m = [regex]::Match($ms[$ms.Count - 1].Line, $Pattern)
    if (-not $m.Success) { return '' }
    return $m.Groups[$Group].Value
}

# first regex match of a group from a log file ('' when absent)
function First-Match([string]$Path, [string]$Pattern, [int]$Group = 1) {
    if (-not (Test-Path $Path)) { return '' }
    $m = [regex]::Match((Get-Content -LiteralPath $Path -Raw), $Pattern)
    if (-not $m.Success) { return '' }
    return $m.Groups[$Group].Value
}

# line number of the first line matching a regex (0 when absent)
function First-LineNumber([string]$Path, [string]$Pattern) {
    if (-not (Test-Path $Path)) { return 0 }
    $ms = @(Select-String -Path $Path -Pattern $Pattern -ErrorAction SilentlyContinue)
    if ($ms.Count -eq 0) { return 0 }
    return $ms[0].LineNumber
}

# masked address inside [start, end)
function In-Range([uint32]$Addr, [uint32]$Start, [uint32]$End) {
    $a = $Addr -band 0xFFFFFFFE
    return ($a -ge ($Start -band 0xFFFFFFFE)) -and ($a -lt ($End -band 0xFFFFFFFE))
}

# count single-step stops (stopReason=3) whose PC is inside [lo, hi)
function Count-StepStopsInRange([string]$Path, [uint32]$Lo, [uint32]$Hi) {
    if (-not (Test-Path $Path)) { return 0 }
    $n = 0
    foreach ($l in (Get-Content -LiteralPath $Path)) {
        if ($l -match 'stopReason=3 pc=0x([0-9A-Fa-f]+)') {
            if (In-Range ([Convert]::ToUInt32($Matches[1], 16)) $Lo $Hi) { $n++ }
        }
    }
    return $n
}

# count breakpoint stops (stopReason=2) at one address
function Count-BpStopsAt([string]$Path, [uint32]$Addr) {
    if (-not (Test-Path $Path)) { return 0 }
    $target = $Addr -band 0xFFFFFFFE
    $n = 0
    foreach ($l in (Get-Content -LiteralPath $Path)) {
        if ($l -match 'stopReason=2 pc=0x([0-9A-Fa-f]+)') {
            if (([Convert]::ToUInt32($Matches[1], 16) -band 0xFFFFFFFE) -eq $target) { $n++ }
        }
    }
    return $n
}

# ---- session runner ------------------------------------------------------
# Async variant (B.4.3 spec 66/67): start a session, let the CALLER interact
# with the running target (probe HALT / kill the simulator), then collect the
# evidence in Complete-KeilSessionAsync.
function Start-KeilSessionAsync([string]$tag, [bool]$LdApp, [bool]$Gomain,
                                [string[]]$Init, [string]$Pipe) {
    Set-UvoptxOptions $uvoptx $LdApp $Gomain '.\exit_init.ini'
    $text = ($Init -join "`r`n")
    if ($text.Length -gt 0) { $text += "`r`n" }
    [System.IO.File]::WriteAllText($initFile, $text, (New-Object System.Text.ASCIIEncoding))
    $logLen = 0
    if (Test-Path $agdiLog) { $logLen = (Get-Item $agdiLog).Length }
    $out = Join-Path $outDir ("{0}_uv4.txt" -f $tag)
    if (Test-Path $out) { Remove-Item $out -Force }
    $pr = Start-Process -FilePath $uv4 -ArgumentList @('-d', $proj, '-j0', '-sg', ('-o' + $out)) `
                        -PassThru -WindowStyle Hidden
    return [pscustomobject]@{ Proc = $pr; LogLen = $logLen; Tag = $tag }
}

function Complete-KeilSessionAsync($async, [int]$TimeoutSec) {
    $finished = $async.Proc.WaitForExit($TimeoutSec * 1000)
    if (-not $finished) { $async.Proc.Kill(); Start-Sleep -Milliseconds 600 }
    $evidence = Join-Path $outDir ("{0}_agdi.log" -f $async.Tag)
    $delta = Read-LogDelta $agdiLog $async.LogLen
    if ($delta.Length -gt 0) { Set-Content -LiteralPath $evidence -Value $delta -Encoding UTF8 }
    return [pscustomobject]@{
        Exit = if ($finished) { $async.Proc.ExitCode } else { 'TIMEOUT' }
        Evidence = $evidence
    }
}

# Runs one real UV4 debug session with the given uvoptx flags + init-file
# commands.  With -CaptureCli the init file also opens a LOG so that the
# Command-window output (EVAL results etc.) is captured to <tag>_cli.txt.
function Invoke-KeilSession([string]$tag, [bool]$LdApp, [bool]$Gomain,
                            [string[]]$Init, [string]$Pipe, [bool]$CaptureCli = $false) {
    Set-UvoptxOptions $uvoptx $LdApp $Gomain '.\exit_init.ini'
    $cmds = @($Init)
    $cli = Join-Path $outDir ("{0}_cli.txt" -f $tag)
    if ($CaptureCli) {
        if (Test-Path $cli) { Remove-Item $cli -Force }
        $cmds = @('LOG > ' + $cli) + $Init + @('LOG OFF')
    }
    $text = ($cmds -join "`r`n")
    if ($text.Length -gt 0) { $text += "`r`n" }
    [System.IO.File]::WriteAllText($initFile, $text, (New-Object System.Text.ASCIIEncoding))

    $logLen = 0
    if (Test-Path $agdiLog) { $logLen = (Get-Item $agdiLog).Length }
    $out = Join-Path $outDir ("{0}_uv4.txt" -f $tag)
    if (Test-Path $out) { Remove-Item $out -Force }

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $pr = Start-Process -FilePath $uv4 -ArgumentList @('-d', $proj, '-j0', '-sg', ('-o' + $out)) `
                        -PassThru -WindowStyle Hidden
    $finished = $pr.WaitForExit($TimeoutSec * 1000)
    if (-not $finished) { $pr.Kill(); Start-Sleep -Milliseconds 600 }
    $sw.Stop()

    $evidence = Join-Path $outDir ("{0}_agdi.log" -f $tag)
    $delta = Read-LogDelta $agdiLog $logLen
    if ($delta.Length -gt 0) { Set-Content -LiteralPath $evidence -Value $delta -Encoding UTF8 }
    $cliText = if ((Test-Path $cli)) { Get-Content -LiteralPath $cli -Raw } else { '' }
    return [pscustomobject]@{
        Exit = if ($finished) { $pr.ExitCode } else { 'TIMEOUT' }
        Sec = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
        Evidence = $evidence
        Cli = $cli
        CliText = $cliText
        Pipe = $Pipe
    }
}

# ---- fixture patches (under the backup + finally restore) ----------------
function Set-SourceVariant([string]$Variant) {
    $t = [System.IO.File]::ReadAllText($mainC)
    if ($Variant -eq 'A') {
        $t = [regex]::Replace($t, 'BUILD_MAGIC_VALUE\s+0x[0-9A-Fa-f]+u', 'BUILD_MAGIC_VALUE   0x11111111u')
        $t = [regex]::Replace($t, 'BUILD_VARIANT_VALUE\s+0x[0-9A-Fa-f]+u', 'BUILD_VARIANT_VALUE 0x11u')
    } else {
        $t = [regex]::Replace($t, 'BUILD_MAGIC_VALUE\s+0x[0-9A-Fa-f]+u', 'BUILD_MAGIC_VALUE   0x22222222u')
        $t = [regex]::Replace($t, 'BUILD_VARIANT_VALUE\s+0x[0-9A-Fa-f]+u', 'BUILD_VARIANT_VALUE 0x22u')
    }
    [System.IO.File]::WriteAllText($mainC, $t, (New-Object System.Text.UTF8Encoding($false)))
}

function Add-MainCInsert([string]$Snippet) {
    $t = [System.IO.File]::ReadAllText($mainC)
    $t = $t.Replace('#include <stdint.h>', "#include <stdint.h>`r`n`r`n$Snippet")
    [System.IO.File]::WriteAllText($mainC, $t, (New-Object System.Text.UTF8Encoding($false)))
}

function Assert-Build([string]$tag, $Build) {
    if ($Build.Errors -ne 0) {
        throw "build $tag failed ($($Build.Errors) error(s), see build_$tag.txt)"
    }
}

# ---- step anchors (test oracle) ------------------------------------------
# Derives the build-scoped step anchors from µVision's OWN step behavior.
# MEASURED (5.43, hidden session -> disassembly step context):
#   * "P" steps one machine instruction (AG_NSTEP(1)) even AT a call -- the P
#     at the BL therefore executes the call and lands in the callee (that is
#     the headless equivalent of Step Into);
#   * "O" is the real source-level Step Out: µVision reads LR and runs to the
#     return address (AG_GOTILADR + temporary breakpoint) -- the target is the
#     caller's next instruction, i.e. the very address the editor-context
#     Step Over uses as well.
# callSite  = the PC right before the first step that landed in the callee.
# callReturn = the GOTILADR target of the O step (= instruction after the call).
# The DRIVER never does any of this -- test oracle only.
function Get-StepAnchors([string]$Pipe, [string]$Tag, [uint32]$AoAddr, [uint32]$BarAddr) {
    $cmds = @('BS foo', 'G') + @(1..13 | ForEach-Object { 'P' }) + @('O', 'EXIT')
    $ses = Invoke-KeilSession ($Tag + '-walk') $true $false $cmds $Pipe
    $ev = $ses.Evidence
    $pcs = New-Object System.Collections.Generic.List[string]
    $callReturn = ''
    if (Test-Path $ev) {
        foreach ($l in (Get-Content -LiteralPath $ev)) {
            if ($l -match 'stopReason=3 pc=0x([0-9A-Fa-f]+)') { $pcs.Add($Matches[1]) }
            elseif ($l -match 'AG_GOTILADR\) nSteps=0 adr=0x([0-9A-Fa-f]+) start') {
                if ($callReturn -eq '') { $callReturn = $Matches[1] }
            }
        }
    }
    $callSite = ''
    for ($i = 1; $i -lt $pcs.Count; $i++) {
        if (In-Range ([Convert]::ToUInt32($pcs[$i], 16)) $AoAddr $BarAddr) {
            $callSite = $pcs[$i - 1]; break
        }
    }
    return [pscustomobject]@{ Session = $ses; Evidence = $ev; CallSite = $callSite
                              CallReturn = $callReturn; Steps = $pcs }
}

# ==========================================================================
# environment: backup + patch agdi.ini (restored in the outer finally)
# ==========================================================================
Stop-TestUv4
Stop-TestSim $PipeName

$iniExisted = Test-Path $agdiIni
$iniBackup = "$agdiIni.b4src.bak"
if ($iniExisted) { Copy-Item -LiteralPath $agdiIni -Destination $iniBackup -Force }
$lines = @()
if ($iniExisted) { $lines = @(Get-Content -LiteralPath $agdiIni) }
if (-not ($lines | Where-Object { $_ -match '^\s*\[BlueBridge\]' })) {
    $lines = @('[BlueBridge]') + $lines
}
$sawAuto = $false; $sawPipe = $false; $sawTrace = $false; $sawLoad = $false
$patched = foreach ($l in $lines) {
    if     ($l -match '^\s*AutoStart\s*=') { $sawAuto = $true; 'AutoStart=0' }
    elseif ($l -match '^\s*PipeName\s*=')  { $sawPipe = $true; "PipeName=$PipeName" }
    elseif ($l -match '^\s*Trace\s*=')     { $sawTrace = $true; 'Trace=1' }
    elseif ($l -match '^\s*LoadTrace\s*=') { $sawLoad = $true; 'LoadTrace=1' }
    else                                   { $l }
}
if (-not $sawAuto)  { $patched += 'AutoStart=0' }
if (-not $sawPipe)  { $patched += "PipeName=$PipeName" }
if (-not $sawTrace) { $patched += 'Trace=1' }
if (-not $sawLoad)  { $patched += 'LoadTrace=1' }
[System.IO.File]::WriteAllLines($agdiIni, $patched, (New-Object System.Text.UTF8Encoding($false)))

$uvoptxBackup = "$uvoptx.b4src.bak"
$uvprojxBackup = "$uvprojx.b4src.bak"
$mainBackup   = "$mainC.b4src.bak"
$initBackup   = "$initFile.b4src.bak"
Copy-Item -LiteralPath $uvoptx -Destination $uvoptxBackup -Force
Copy-Item -LiteralPath $uvprojx -Destination $uvprojxBackup -Force
Copy-Item -LiteralPath $mainC -Destination $mainBackup -Force
Copy-Item -LiteralPath $initFile -Destination $initBackup -Force

$simProc = $null
$rows = @()
try {
    foreach ($s in $Scenarios) {
        $script:checks = @()
        Write-Host ("=== scenario {0}" -f $s) -ForegroundColor Cyan

        # every scenario starts from the pristine fixture + its own patches
        Copy-Item -LiteralPath $mainBackup -Destination $mainC -Force

        try {
        switch ($s) {

        'run-main' {
            # spec 18-20: fresh simulator (flash FF) -> Build -> Start Debug ->
            # real LoadApp -> reset -> Run to main -> stop at main.
            Set-SourceVariant 'A'
            $b = Invoke-KeilBuild 'runmain' $outDir
            Check 'build A: 0 errors' ($b.Errors -eq 0)
            Assert-Build 'runmain' $b
            $mainAddr = Get-MapSymbol $mapFile 'main'
            Check 'map oracle: main found' ($null -ne $mainAddr) `
                  ("main=0x{0:X8}" -f $mainAddr)

            $simProc = Start-TestSim $PipeName
            $pre = Read-MemBytes $PipeName 0x08000000 64
            Check 'fresh flash is 0xFF before the session (no preload)' `
                  (($pre | Where-Object { $_ -ne 0xFF }).Count -eq 0)

            $ses = Invoke-KeilSession 'run-main' $true $true @('EXIT') $PipeName
            Check 'session exit=0' ($ses.Exit -eq 0) ("exit=$($ses.Exit) ($($ses.Sec)s)")
            $ev = $ses.Evidence
            Check 'real Keil download (PROGRAM_END exactly once)' `
                  ((Count-Marker $ev '[PROGRAM] end success') -eq 1)
            Check 'no NOCODE in the normal flow' ((Count-Marker $ev 'noCode=1') -eq 0)
            Check 'uVision requested AG_GOTILADR for run-to-main' `
                  ((Count-Regex $ev 'AG_GoStep.*AG_GOTILADR') -ge 2) `
                  ("lines=$(Count-Regex $ev 'AG_GoStep.*AG_GOTILADR')")
            $reqAddr = Last-Match $ev 'AG_GOTILADR\) nSteps=0 adr=0x([0-9A-Fa-f]+) start'
            Check 'requested address == MAP main (uVision resolved it)' `
                  ($reqAddr -ne '' -and
                   ([Convert]::ToUInt32($reqAddr, 16) -band 0xFFFFFFFE) -eq ($mainAddr -band 0xFFFFFFFE)) `
                  ("requested=0x$reqAddr main=0x{0:X8}" -f $mainAddr)
            $stopPc = Last-Match $ev 'stopped: Breakpoint pc=0x([0-9A-Fa-f]+)'
            Check 'stopped at main instruction' `
                  ($stopPc -ne '' -and
                   ([Convert]::ToUInt32($stopPc, 16) -band 0xFFFFFFFE) -eq ($mainAddr -band 0xFFFFFFFE)) `
                  ("pc=0x$stopPc")
            Check 'run-to-main did not stop at reset/other address' `
                  ($stopPc -ne '' -and
                   ([Convert]::ToUInt32($stopPc, 16) -band 0xFFFFFFFE) -ne 0x0800010C) `
                  ("pc=0x$stopPc resetPc=0x0800010C")
            Check 'temporary breakpoint installed then removed (backend=0)' `
                  (((Count-Regex $ev 'temporary breakpoint 0x[0-9A-Fa-f]+ installed \(backend=1\)') -ge 1) -and
                   ((Count-Regex $ev 'temporary breakpoint 0x[0-9A-Fa-f]+ removed \(backend=0\)') -ge 1))
            Check 'target stayed halted after the stop' `
                  ((Read-State $PipeName).State -ne 1) `
                  ("state=" + (Read-State $PipeName).State)
            $regs = Read-Regs $PipeName
            Check 'PC register matches the stop PC' `
                  ($regs.ContainsKey('pc') -and ($regs['pc'] -band 0xFFFFFFFE) -eq ([Convert]::ToUInt32($stopPc, 16) -band 0xFFFFFFFE)) `
                  ("regs.pc=0x{0:X8}" -f $regs['pc'])
            Check 'SP is inside SRAM/CCM' `
                  ($regs.ContainsKey('sp') -and
                   (($regs['sp'] -ge 0x20000000 -and $regs['sp'] -lt 0x20008000) -or
                    ($regs['sp'] -ge 0x10000000 -and $regs['sp'] -lt 0x10004000))) `
                  ("sp=0x{0:X8}" -f $regs['sp'])
            Check 'no connection loss' ((Count-Marker $ev 'connection lost') -eq 0)
        }

        'run-main-ab' {
            # spec 21: build A -> run-to-main; then a build whose `main` moved
            # -> run-to-main again must use the NEW address (no cached one).
            Set-SourceVariant 'A'
            $b = Invoke-KeilBuild 'rmab-A' $outDir
            Check 'build A: 0 errors' ($b.Errors -eq 0)
            Assert-Build 'rmab-A' $b
            $mainA = Get-MapSymbol $mapFile 'main'
            $simProc = Start-TestSim $PipeName

            $sesA = Invoke-KeilSession 'rmab-a' $true $true @('EXIT') $PipeName
            Check 'A session exit=0' ($sesA.Exit -eq 0)
            $pcA = Last-Match $sesA.Evidence 'stopped: Breakpoint pc=0x([0-9A-Fa-f]+)'
            Check 'A stopped at A-main' `
                  ($pcA -ne '' -and
                   ([Convert]::ToUInt32($pcA, 16) -band 0xFFFFFFFE) -eq ($mainA -band 0xFFFFFFFE)) `
                  ("pc=0x$pcA map=0x{0:X8}" -f $mainA)

            # pad the image so `main` moves (alphabetically first text section)
            Add-MainCInsert @'
/* B.4.3 run-main-ab padding: shifts every later .text section (incl. main) */
__attribute__((used, noinline)) int a_pad_b43(int x)
{
    volatile int v1 = x + 1;
    volatile int v2 = v1 * 3;
    volatile int v3 = v2 + 7;
    return v3;
}
'@
            $b2 = Invoke-KeilBuild 'rmab-B' $outDir
            Check 'build B: 0 errors' ($b2.Errors -eq 0)
            Assert-Build 'rmab-B' $b2
            $mainB = Get-MapSymbol $mapFile 'main'
            Check 'main address really moved in build B' ($mainA -ne $mainB) `
                  ("A=0x{0:X8} B=0x{1:X8}" -f $mainA, $mainB)

            $sesB = Invoke-KeilSession 'rmab-b' $true $true @('EXIT') $PipeName
            Check 'B session exit=0' ($sesB.Exit -eq 0)
            $pcB = Last-Match $sesB.Evidence 'stopped: Breakpoint pc=0x([0-9A-Fa-f]+)'
            Check 'B stopped at B-main (no cached address)' `
                  ($pcB -ne '' -and
                   ([Convert]::ToUInt32($pcB, 16) -band 0xFFFFFFFE) -eq ($mainB -band 0xFFFFFFFE) -and
                   ([Convert]::ToUInt32($pcB, 16) -band 0xFFFFFFFE) -ne ($mainA -band 0xFFFFFFFE)) `
                  ("pc=0x$pcB Bmain=0x{0:X8} Amain=0x{1:X8}" -f $mainB, $mainA)
            Check 'both downloads committed' `
                  (((Count-Marker $sesA.Evidence '[PROGRAM] end success') -eq 1) -and
                   ((Count-Marker $sesB.Evidence '[PROGRAM] end success') -eq 1))
        }

        'source-break' {
            # spec 60/61: after a REAL LoadApp (no NOCODE) the symbol table is
            # µVision's own; a source breakpoint resolves + hits, and the
            # run-to-main request still comes (deferred until after the
            # init-file commands -- measured 5.43 behavior).
            Set-SourceVariant 'A'
            $b = Invoke-KeilBuild 'source-break' $outDir
            Check 'build: 0 errors' ($b.Errors -eq 0)
            Assert-Build 'source-break' $b
            $fooAddr = Get-MapSymbol $mapFile 'foo'
            $mainAddr = Get-MapSymbol $mapFile 'main'
            $simProc = Start-TestSim $PipeName
            $ses = Invoke-KeilSession 'source-break' $true $true @('BS foo', 'G', 'EXIT') $PipeName
            Check 'session exit=0' ($ses.Exit -eq 0) ("exit=$($ses.Exit)")
            $ev = $ses.Evidence
            Check 'real download (no preload)' ((Count-Marker $ev '[PROGRAM] end success') -eq 1)
            $nocodeHits = Count-Marker $ev 'noCode=1'
            Check 'no NOCODE workaround' ($nocodeHits -eq 0) ("hits=$nocodeHits")
            $bpAddr = First-Match $ev 'Bp link 0x([0-9A-Fa-f]+) handle'
            Check 'uVision resolved BS foo from the AXF itself' `
                  ($bpAddr -ne '' -and
                   ([Convert]::ToUInt32($bpAddr, 16) -band 0xFFFFFFFE) -eq ($fooAddr -band 0xFFFFFFFE)) `
                  ("bp=0x$bpAddr map foo=0x{0:X8}" -f $fooAddr)
            $pc1 = First-Match $ev 'stopped: Breakpoint pc=0x([0-9A-Fa-f]+)'
            Check 'first stop is foo' `
                  ($pc1 -ne '' -and
                   ([Convert]::ToUInt32($pc1, 16) -band 0xFFFFFFFE) -eq ($fooAddr -band 0xFFFFFFFE)) `
                  ("pc=0x$pc1")
            $goto = First-Match $ev 'AG_GOTILADR\) nSteps=0 adr=0x([0-9A-Fa-f]+) start'
            Check 'run-to-main still requested after the BP work' `
                  ($goto -ne '' -and
                   ([Convert]::ToUInt32($goto, 16) -band 0xFFFFFFFE) -eq ($mainAddr -band 0xFFFFFFFE)) `
                  ("gotiladr=0x$goto main=0x{0:X8}" -f $mainAddr)
            $lnBp = First-LineNumber $ev 'Bp link'
            $lnGo = First-LineNumber $ev 'AG_GOTILADR\) nSteps=0'
            Check 'run-to-main is deferred until after the init-file commands' `
                  ($lnBp -gt 0 -and $lnGo -gt $lnBp) ("bpLine=$lnBp gotiladrLine=$lnGo")
            Check 'no connection loss' ((Count-Marker $ev 'connection lost') -eq 0) `
                  ("hits=" + (Count-Marker $ev 'connection lost'))
        }

        'watch-global' {
            # spec 23-26/37: µVision's own expression engine (the Watch window
            # backend) reads the globals through the target primitives; every
            # value is cross-checked against a raw IPC read of the same address.
            Set-SourceVariant 'A'
            $b = Invoke-KeilBuild 'watch' $outDir
            Check 'build: 0 errors' ($b.Errors -eq 0)
            Assert-Build 'watch' $b
            $gcAddr = Get-MapSymbol $mapFile 'g_counter'
            $grAddr = Get-MapSymbol $mapFile 'g_result'
            $simProc = Start-TestSim $PipeName
            # Gomain = false: no deferred run-to-main, so the target stays at
            # the second breakpoint stop for the post-session IPC cross-check
            $ses = Invoke-KeilSession 'watch-global' $true $false @(
                'BS bar',
                'G',
                'EVAL g_counter',
                'EVAL g_counter',
                'EVAL g_counter + 1',
                'EVAL g_result',
                'EVAL g_step_marker',
                'EVAL &g_counter',
                'G',
                'EVAL g_counter',
                'EVAL g_counter + 1',
                'EVAL g_result',
                'EXIT') $PipeName $true
            Check 'session exit=0' ($ses.Exit -eq 0) ("exit=$($ses.Exit)")
            $ev = $ses.Evidence
            $cli = $ses.CliText
            Check 'two breakpoint stops (watch while running vs halted)' `
                  ((Count-Marker $ev 'stopped: Breakpoint') -eq 2) `
                  ("stops=" + (Count-Marker $ev 'stopped: Breakpoint'))
            $errHits = Count-Marker $ev 'error 123'
            Check 'no AGDI error 123 on the watch path' `
                  (($errHits -eq 0) -and ($ses.Exit -eq 0)) ("hits=$errHits exit=$($ses.Exit)")
            # Keil's own values (first stop: main seeded g_counter from build_magic)
            Check 'Keil: g_counter == 0x11111111 at the 1st stop' ($cli -match '0x11111111')
            Check 'Keil: value stable while halted (evaluated twice)' `
                  (($cli | Select-String -Pattern '0x11111111' -AllMatches).Matches.Count -ge 2)
            Check 'Keil: g_counter == 0x11111112 at the 2nd stop' ($cli -match '0x11111112')
            Check 'Keil: expression g_counter + 1 evaluated (0x11111113)' ($cli -match '0x11111113')
            Check 'Keil: g_result == 32 (bar(1) = ((1+20)+10)+1) after one pass' ($cli -match '0x00000020')
            Check 'Keil: &g_counter resolves to the MAP address' `
                  ($gcAddr -ne $null -and $cli -match ('0x{0:X8}' -f $gcAddr)) `
                  ("map=0x{0:X8}" -f $gcAddr)
            # raw IPC reads of the same addresses (Keil value == target memory)
            Check 'IPC g_counter == 0x11111112 (matches Keil)' ((Read-Mem32 $PipeName $gcAddr) -eq 0x11111112) `
                  ("ipc=0x{0:X8}" -f (Read-Mem32 $PipeName $gcAddr))
            Check 'IPC g_result == 32 (matches Keil)' ((Read-Mem32 $PipeName $grAddr) -eq 32) `
                  ("ipc=0x{0:X8}" -f (Read-Mem32 $PipeName $grAddr))
            # a fresh real LoadApp re-downloads and RESETS: the program restarts,
            # so the very first stop sees the seeded values again (spec 19: the
            # session never continues a preloaded image)
            $ses2 = Invoke-KeilSession 'watch-global-run' $true $false @('BS bar', 'G', 'EXIT') $PipeName
            Check 'second session (fresh download) exit=0' ($ses2.Exit -eq 0)
            Check 'fresh LoadApp restarted the program (g_counter seeded again)' `
                  ((Read-Mem32 $PipeName $gcAddr) -eq 0x11111111) `
                  ("ipc=0x{0:X8}" -f (Read-Mem32 $PipeName $gcAddr))
        }

        'locals' {
            # spec 27/28/29: stop inside add_one, walk past the prologue, then
            # let µVision evaluate the parameter and the local.  Cross-check with
            # the raw stack words (post-session, target halted) and with the
            # source logic (x = 31, local = x + 1 = 32 for the first call).
            Set-SourceVariant 'A'
            $b = Invoke-KeilBuild 'locals' $outDir
            Check 'build: 0 errors' ($b.Errors -eq 0)
            Assert-Build 'locals' $b
            $aoAddr = Get-MapSymbol $mapFile 'add_one'
            $barAddr = Get-MapSymbol $mapFile 'bar'
            $simProc = Start-TestSim $PipeName
            $ses = Invoke-KeilSession 'locals' $true $false @(
                'BS add_one',
                'G',
                'P', 'P', 'P', 'P', 'P',          # SUB/STR x/LDR/ADDS/STR local
                'EVAL x',
                'EVAL local',
                'EVAL g_step_marker',
                'P', 'P', 'P', 'P',               # marker line executed
                'EVAL g_step_marker',
                'EVAL local',
                'EVAL x',
                'EXIT') $PipeName $true
            Check 'session exit=0' ($ses.Exit -eq 0) ("exit=$($ses.Exit)")
            $ev = $ses.Evidence
            $pc1 = First-Match $ev 'stopped: Breakpoint pc=0x([0-9A-Fa-f]+)'
            Check 'stopped at add_one (source BP resolved by uVision)' `
                  ($pc1 -ne '' -and
                   ([Convert]::ToUInt32($pc1, 16) -band 0xFFFFFFFE) -eq ($aoAddr -band 0xFFFFFFFE)) `
                  ("pc=0x$pc1 map=0x{0:X8}" -f $aoAddr)
            $cli = $ses.CliText
            Check 'Keil Locals: x == 31 (caller passed seed+30)' ($cli -match '0x0000001F')
            Check 'Keil Locals: local == x + 1 == 32' ($cli -match '0x00000020')
            Check 'Keil: marker still 0x22 before the marker line' ($cli -match '0x00000022')
            Check 'Keil: marker becomes 0x11 after the marker line' ($cli -match '0x00000011')
            # raw target-memory cross-check (stack slots [sp+4]=x, [sp+0]=local)
            $regs = Read-Regs $PipeName
            $sp = $regs['sp']
            Check 'PC still inside add_one after the walk' `
                  (In-Range $regs['pc'] $aoAddr $barAddr) ("pc=0x{0:X8}" -f $regs['pc'])
            Check 'target memory [sp+4] == x == 31' ((Read-Mem32 $PipeName ($sp + 4)) -eq 31) `
                  ("[sp+4]=0x{0:X8}" -f (Read-Mem32 $PipeName ($sp + 4)))
            Check 'target memory [sp+0] == local == 32' ((Read-Mem32 $PipeName ($sp + 0)) -eq 32) `
                  ("[sp+0]=0x{0:X8}" -f (Read-Mem32 $PipeName ($sp + 0)))
        }

        'callstack' {
            # spec 31-34/80: the machine-side facts a call-stack view depends on.
            # The Call Stack WINDOW content itself is a MANUAL check (spec 76);
            # here: PC/LR/SP + the raw stack words inside the caller ranges (a
            # coarse oracle -- NOT a claim of equivalence with Keil's unwinder).
            Set-SourceVariant 'A'
            $b = Invoke-KeilBuild 'callstack' $outDir
            Check 'build: 0 errors' ($b.Errors -eq 0)
            Assert-Build 'callstack' $b
            $aoAddr = Get-MapSymbol $mapFile 'add_one'
            $barAddr = Get-MapSymbol $mapFile 'bar'
            $bvAddr = Get-MapSymbol $mapFile 'build_variant'
            $fooAddr = Get-MapSymbol $mapFile 'foo'
            $mainAddr = Get-MapSymbol $mapFile 'main'
            $simProc = Start-TestSim $PipeName
            $ses = Invoke-KeilSession 'callstack' $true $false @('BS add_one', 'G', 'EXIT') $PipeName $true
            Check 'session exit=0' ($ses.Exit -eq 0) ("exit=$($ses.Exit)")
            $regs = Read-Regs $PipeName
            Check 'stopped inside add_one' (In-Range $regs['pc'] $aoAddr $barAddr) `
                  ("pc=0x{0:X8}" -f $regs['pc'])
            Check 'LR points into the caller foo (return path)' `
                  (In-Range $regs['lr'] $fooAddr $mainAddr) ("lr=0x{0:X8}" -f $regs['lr'])
            $sp = $regs['sp']
            Check 'SP is inside SRAM' (($sp -ge 0x20000000) -and ($sp -lt 0x20008000)) `
                  ("sp=0x{0:X8}" -f $sp)
            $stack = Read-MemBytes $PipeName $sp 256
            $inBar = 0; $inMain = 0; $inFoo = 0
            for ($i = 0; $i + 3 -lt $stack.Length; $i += 4) {
                $w = [BitConverter]::ToUInt32($stack, $i)
                if (In-Range $w $barAddr $bvAddr) { $inBar++ }
                elseif (In-Range $w $fooAddr $mainAddr) { $inFoo++ }
                elseif (In-Range $w $mainAddr ($mainAddr + 0x200)) { $inMain++ }
            }
            Check 'stack holds a return address into bar (frame for the unwinder)' `
                  ($inBar -ge 1) ("words=$inBar")
            Check 'stack holds a return address into main (2 frames up)' `
                  ($inMain -ge 1) ("words=$inMain")
            Check 'no AGDI error on the unwind-information path' `
                  ((Count-Marker $ses.Evidence 'error 123') -eq 0) `
                  ("hits=" + (Count-Marker $ses.Evidence 'error 123'))
        }

        'run-main-stress' {
            # spec 20/88: N x (Start Debug -> real LoadApp -> reset -> run to
            # main -> stop session); the same build, one simulator, in place.
            $rounds = if ($Rounds -gt 0) { $Rounds } else { 20 }
            Set-SourceVariant 'A'
            $b = Invoke-KeilBuild 'rmstress' $outDir
            Check 'build: 0 errors' ($b.Errors -eq 0)
            Assert-Build 'rmstress' $b
            $mainAddr = Get-MapSymbol $mapFile 'main'
            $simProc = Start-TestSim $PipeName
            $ok = 0; $badRounds = @()
            $inst = 0; $rem = 0; $commits = 0
            for ($i = 1; $i -le $rounds; $i++) {
                $tag = "rmstress-{0:d2}" -f $i
                $ses = Invoke-KeilSession $tag $true $true @('EXIT') $PipeName
                $ev = $ses.Evidence
                $pc = Last-Match $ev 'stopped: Breakpoint pc=0x([0-9A-Fa-f]+)'
                $good = ($ses.Exit -eq 0) -and ($pc -ne '') -and
                        (([Convert]::ToUInt32($pc, 16) -band 0xFFFFFFFE) -eq ($mainAddr -band 0xFFFFFFFE)) -and
                        ((Count-Marker $ev '[PROGRAM] end success') -eq 1) -and
                        ((Count-Marker $ev 'connection lost') -eq 0)
                if ($good) { $ok++ } else { $badRounds += ("#{0}: exit={1} pc=0x{2}" -f $i, $ses.Exit, $pc) }
                $commits += (Count-Marker $ev '[PROGRAM] end success')
                $inst += (Count-Regex $ev 'temporary breakpoint 0x[0-9A-Fa-f]+ installed')
                $rem  += (Count-Regex $ev 'temporary breakpoint 0x[0-9A-Fa-f]+ removed \(backend=0\)')
            }
            Check ("run-to-main stress: {0}/{1} sessions stopped at main" -f $ok, $rounds) `
                  ($ok -eq $rounds) ($badRounds -join '; ')
            Check 'every round really downloaded (commit each round)' ($commits -eq $rounds) ("commits=$commits")
            Check 'temporary breakpoints cleaned up in every round' `
                  (($inst -eq $rounds) -and ($rem -eq $rounds)) ("installed=$inst removed(backend=0)=$rem")
        }

        'trace-steps' {
            # spec 39 (observation first): what does µVision really send for
            # T / P / O?  Measured with 5.43: T and P-at-normal-instruction use
            # AG_NSTEP(1) (one guest instruction); P at a call reads the opcode
            # (AG_RDOPC) first; O uses AG_GOTILADR with the LR return address +
            # a temporary breakpoint.  This scenario documents that and prints
            # the raw sequence.
            Set-SourceVariant 'A'
            $b = Invoke-KeilBuild 'trace-steps' $outDir
            Check 'build: 0 errors' ($b.Errors -eq 0)
            Assert-Build 'trace-steps' $b
            $mainAddr = Get-MapSymbol $mapFile 'main'
            $fooAddr = Get-MapSymbol $mapFile 'foo'
            $simProc = Start-TestSim $PipeName
            $ses = Invoke-KeilSession 'trace-steps' $true $true @(
                'BS add_one',
                'G',
                'EVAL x',
                'EVAL local',
                'EVAL g_step_marker',
                'T',
                'EVAL local',
                'T',
                'EVAL local',
                'P',
                'EVAL g_step_marker',
                'O',
                'EVAL a',
                'EVAL g_step_marker',
                'EXIT') $PipeName $true
            Check 'session exit=0' ($ses.Exit -eq 0) ("exit=$($ses.Exit) ($($ses.Sec)s)")
            $ev = $ses.Evidence
            Check 'breakpoint stopped inside add_one' `
                  ((Count-Marker $ev 'stopped: Breakpoint') -ge 1)
            Check 'T and P use AG_NSTEP(1) single-instruction steps' `
                  ((Count-Regex $ev 'AG_GoStep nCode=0x02\(AG_NSTEP\) nSteps=1') -ge 3) `
                  ("nstepLines=" + (Count-Regex $ev 'AG_GoStep nCode=0x02\(AG_NSTEP\) nSteps=1'))
            Check 'step out used AG_GOTILADR + temporary breakpoint (user BP kept)' `
                  (((Count-Regex $ev 'AG_GOTILADR\) nSteps=0 adr=0x[0-9A-Fa-f]+ start') -ge 2) -and
                   ((Count-Regex $ev 'temporary breakpoint 0x[0-9A-Fa-f]+ installed \(backend=2\)') -ge 1) -and
                   ((Count-Regex $ev 'temporary breakpoint 0x[0-9A-Fa-f]+ removed \(backend=1\)') -ge 1))
            $oAddr = First-Match $ev 'AG_GOTILADR\) nSteps=0 adr=0x([0-9A-Fa-f]+) start'
            Check 'step-out target is inside the caller foo (LR return address)' `
                  ($oAddr -ne '' -and
                   (In-Range ([Convert]::ToUInt32($oAddr, 16)) $fooAddr $mainAddr)) ("gotiladr=0x$oAddr")
            $cli = $ses.CliText
            Check 'after step-out uVision evaluates the caller local a == 31' ($cli -match '0x0000001F')
            Check 'step-out really left add_one (marker 0x11 written)' ($cli -match '0x00000011')
            # the LAST GOTILADR is the deferred run-to-main (main entry)
            $allGo = @(Select-String -Path $ev -Pattern 'AG_GOTILADR\) nSteps=0 adr=0x([0-9A-Fa-f]+) start')
            $lastGo = if ($allGo.Count -gt 0) { [regex]::Match($allGo[$allGo.Count-1].Line, 'adr=0x([0-9A-Fa-f]+)').Groups[1].Value } else { '' }
            Check 'run-to-main (GOTILADR to main) is the last run of the session' `
                  ($lastGo -ne '' -and
                   ([Convert]::ToUInt32($lastGo, 16) -band 0xFFFFFFFE) -eq ($mainAddr -band 0xFFFFFFFE)) `
                  ("last=0x$lastGo")
            Write-Host "    --- GoStep/BpInfo/BreakFunc sequence:"
            Select-String -Path $ev -Pattern 'AG_GoStep|temporary breakpoint|Bp link|Bp state|stopped:|AG_BreakFunc' |
                ForEach-Object { Write-Host ("      " + ($_.Line -replace '^\S+ \S+  pid=\d+ tid=\d+  ', '')) }
            Write-Host "    --- Command-window (EVAL) output:"
            if ($ses.CliText) { $ses.CliText -split "`r?`n" | Where-Object { $_ } | ForEach-Object { Write-Host ("      " + $_) } }
            Check 'EVAL output captured (Command window log)' `
                  ($ses.CliText.Length -gt 0) ("bytes=$($ses.CliText.Length)")
        }

        'step-walk' {
            # spec 39/43/47: measure how µVision implements source stepping at a
            # CALL instruction.  Measured: "P" steps over normal instructions
            # with AG_NSTEP(1) and switches to AG_GOTILADR + a temporary
            # breakpoint exactly at the call -- the target is the instruction
            # after the call (the caller's next source line).
            Set-SourceVariant 'A'
            $b = Invoke-KeilBuild 'step-walk' $outDir
            Check 'build: 0 errors' ($b.Errors -eq 0)
            Assert-Build 'step-walk' $b
            $fooAddr = Get-MapSymbol $mapFile 'foo'
            $mainAddr = Get-MapSymbol $mapFile 'main'
            $aoAddr = Get-MapSymbol $mapFile 'add_one'
            $barAddr = Get-MapSymbol $mapFile 'bar'
            $simProc = Start-TestSim $PipeName
            $an = Get-StepAnchors $PipeName 'step-walk' $aoAddr $barAddr
            Check 'walk session exit=0' ($an.Session.Exit -eq 0)
            $ev = $an.Evidence
            Check 'anchors derived (call site + return point)' `
                  (($an.CallSite -ne '') -and ($an.CallReturn -ne '')) `
                  ("callSite=0x$($an.CallSite) callReturn=0x$($an.CallReturn)")
            Check 'call site and return point are inside foo' `
                  ((In-Range ([Convert]::ToUInt32($an.CallSite, 16)) $fooAddr $mainAddr) -and
                   (In-Range ([Convert]::ToUInt32($an.CallReturn, 16)) $fooAddr $mainAddr))
            $delta = ([Convert]::ToUInt32($an.CallReturn, 16)) - ([Convert]::ToUInt32($an.CallSite, 16))
            Check 'call instruction is a 2/4-byte branch' ($delta -in @(2, 4)) ("delta=$delta")
            Check 'P/T steps are single instructions (AG_NSTEP(1))' `
                  ((Count-Regex $ev 'AG_GoStep nCode=0x02\(AG_NSTEP\) nSteps=1') -ge 13) `
                  ("nstep=" + (Count-Regex $ev 'AG_GoStep nCode=0x02\(AG_NSTEP\) nSteps=1'))
            Check 'the step at the call executed it and landed in the callee' `
                  ((Count-StepStopsInRange $ev $aoAddr $barAddr) -eq 1) `
                  ("calleeStops=" + (Count-StepStopsInRange $ev $aoAddr $barAddr))
            Check 'step out computed the caller return address (AG_GOTILADR + temp BP)' `
                  (((Count-Regex $ev 'AG_GOTILADR\) nSteps=0 adr=0x[0-9A-Fa-f]+ start') -eq 1) -and
                   ((Count-Regex $ev 'temporary breakpoint 0x[0-9A-Fa-f]+ installed \(backend=2\)') -eq 1) -and
                   ((Count-Regex $ev 'temporary breakpoint 0x[0-9A-Fa-f]+ removed \(backend=1\)') -eq 1))
        }

        'step-into' {
            # spec 40/55/56: run to the call site, then step into -- every time
            # the step must execute the call and land inside the callee.
            $cycles = if ($Rounds -gt 0) { $Rounds } else { 100 }
            Set-SourceVariant 'A'
            $b = Invoke-KeilBuild 'step-into' $outDir
            Check 'build: 0 errors' ($b.Errors -eq 0)
            Assert-Build 'step-into' $b
            $fooAddr = Get-MapSymbol $mapFile 'foo'
            $mainAddr = Get-MapSymbol $mapFile 'main'
            $aoAddr = Get-MapSymbol $mapFile 'add_one'
            $barAddr = Get-MapSymbol $mapFile 'bar'
            $simProc = Start-TestSim $PipeName
            $an = Get-StepAnchors $PipeName 'step-into' $aoAddr $barAddr
            Check 'anchors derived' (($an.CallSite -ne '') -and ($an.CallReturn -ne ''))
            $cs = [Convert]::ToUInt32($an.CallSite, 16)
            $cmds = @()
            for ($i = 0; $i -lt $cycles; $i++) {
                $cmds += ('G,0x{0:X8}' -f $cs)
                $cmds += 'T'
            }
            $cmds += 'EXIT'
            $ses = Invoke-KeilSession 'step-into' $true $false $cmds $PipeName
            Check 'session exit=0' ($ses.Exit -eq 0) ("exit=$($ses.Exit) ($($ses.Sec)s)")
            $ev = $ses.Evidence
            $into = Count-StepStopsInRange $ev $aoAddr $barAddr
            Check ("step into entered the callee every time ({0}/{1})" -f $into, $cycles) ($into -eq $cycles)
            $allSteps = Count-Regex $ev 'stopReason=3 pc='
            Check 'no stray single-step outside the callee' (($allSteps - $into) -eq 0) `
                  ("steps=$allSteps inside=$into")
            Check 'every run-to-call-site used + released a temporary BP' `
                  (((Count-Regex $ev 'temporary breakpoint 0x[0-9A-Fa-f]+ installed \(backend=1\)') -eq $cycles) -and
                   ((Count-Regex $ev 'temporary breakpoint 0x[0-9A-Fa-f]+ removed \(backend=0\)') -eq $cycles)) `
                  ("inst=" + (Count-Regex $ev 'temporary breakpoint 0x[0-9A-Fa-f]+ installed \(backend=1\)') +
                   " rem=" + (Count-Regex $ev 'temporary breakpoint 0x[0-9A-Fa-f]+ removed \(backend=0\)'))
            Check 'no connection loss / timeout' `
                  (((Count-Marker $ev 'connection lost') -eq 0) -and ($ses.Exit -eq 0))
        }

        'step-over' {
            # spec 43/44/56: the Step Over MECHANISM at a call -- run to the
            # caller's next instruction with a temporary breakpoint.  This is
            # the AG_GOTILADR + temp-BP path µVision uses for the source-level
            # Step Over (editor context); in a hidden session the "P" command
            # itself is an instruction step (measured in step-walk), so the
            # mechanism is exercised with the measured return-point address.
            # The callee is executed BY THE RUN, but no STEP may land inside it.
            $cycles = if ($Rounds -gt 0) { $Rounds } else { 100 }
            Set-SourceVariant 'A'
            $b = Invoke-KeilBuild 'step-over' $outDir
            Check 'build: 0 errors' ($b.Errors -eq 0)
            Assert-Build 'step-over' $b
            $fooAddr = Get-MapSymbol $mapFile 'foo'
            $mainAddr = Get-MapSymbol $mapFile 'main'
            $aoAddr = Get-MapSymbol $mapFile 'add_one'
            $barAddr = Get-MapSymbol $mapFile 'bar'
            $simProc = Start-TestSim $PipeName
            $an = Get-StepAnchors $PipeName 'step-over' $aoAddr $barAddr
            Check 'anchors derived' (($an.CallSite -ne '') -and ($an.CallReturn -ne ''))
            $cs = [Convert]::ToUInt32($an.CallSite, 16)
            $cr = [Convert]::ToUInt32($an.CallReturn, 16)
            $cmds = @()
            for ($i = 0; $i -lt $cycles; $i++) {
                $cmds += ('G,0x{0:X8}' -f $cs)   # run to the call site (temp BP)
                $cmds += ('G,0x{0:X8}' -f $cr)   # step over: land on the caller's next instruction
            }
            $cmds += 'EXIT'
            $ses = Invoke-KeilSession 'step-over' $true $false $cmds $PipeName
            Check 'session exit=0' ($ses.Exit -eq 0) ("exit=$($ses.Exit) ($($ses.Sec)s)")
            $ev = $ses.Evidence
            $landings = Count-BpStopsAt $ev $cr
            Check ("step over landed on the caller's next instruction every time ({0}/{1})" -f $landings, $cycles) `
                  ($landings -eq $cycles)
            Check 'no STEP ever landed inside the callee (pure run + temp BP)' `
                  ((Count-Regex $ev 'stopReason=3 pc=') -eq 0) `
                  ("stepStops=" + (Count-Regex $ev 'stopReason=3 pc='))
            Check 'every step-over used AG_GOTILADR to the return point' `
                  ((Count-Regex $ev ('AG_GOTILADR\) nSteps=0 adr=0x{0:X8} start' -f $cr)) -eq $cycles) `
                  ("lines=" + (Count-Regex $ev ('AG_GOTILADR\) nSteps=0 adr=0x{0:X8} start' -f $cr)))
            Check 'temporary BP cleanup for both runs of every cycle (2 -> 0)' `
                  (((Count-Regex $ev 'temporary breakpoint 0x[0-9A-Fa-f]+ installed \(backend=1\)') -eq (2 * $cycles)) -and
                   ((Count-Regex $ev 'temporary breakpoint 0x[0-9A-Fa-f]+ removed \(backend=0\)') -eq (2 * $cycles)))
            Check 'no connection loss / timeout' `
                  (((Count-Marker $ev 'connection lost') -eq 0) -and ($ses.Exit -eq 0))
        }

        'step-out' {
            # spec 45/46/57: from inside the callee, step out -- µVision runs to
            # the LR return address (AG_GOTILADR) and stops in the caller.
            $cycles = if ($Rounds -gt 0) { $Rounds } else { 100 }
            Set-SourceVariant 'A'
            $b = Invoke-KeilBuild 'step-out' $outDir
            Check 'build: 0 errors' ($b.Errors -eq 0)
            Assert-Build 'step-out' $b
            $fooAddr = Get-MapSymbol $mapFile 'foo'
            $mainAddr = Get-MapSymbol $mapFile 'main'
            $aoAddr = Get-MapSymbol $mapFile 'add_one'
            $barAddr = Get-MapSymbol $mapFile 'bar'
            $simProc = Start-TestSim $PipeName
            $an = Get-StepAnchors $PipeName 'step-out' $aoAddr $barAddr
            Check 'anchors derived' (($an.CallSite -ne '') -and ($an.CallReturn -ne ''))
            $cs = [Convert]::ToUInt32($an.CallSite, 16)
            $cr = [Convert]::ToUInt32($an.CallReturn, 16)
            $cmds = @()
            for ($i = 0; $i -lt $cycles; $i++) {
                $cmds += ('G,0x{0:X8}' -f $cs)
                $cmds += 'T'      # enter the callee
                $cmds += 'O'      # step out again
            }
            $cmds += 'EXIT'
            $ses = Invoke-KeilSession 'step-out' $true $false $cmds $PipeName
            Check 'session exit=0' ($ses.Exit -eq 0) ("exit=$($ses.Exit) ($($ses.Sec)s)")
            $ev = $ses.Evidence
            $into = Count-StepStopsInRange $ev $aoAddr $barAddr
            Check ("stepped into the callee every cycle ({0}/{1})" -f $into, $cycles) ($into -eq $cycles)
            $outStops = Count-BpStopsAt $ev $cr
            Check ("step out returned to the caller every time ({0}/{1})" -f $outStops, $cycles) ($outStops -eq $cycles)
            $inStops = Count-BpStopsAt $ev $cs
            Check ("run-to-call-site stops every cycle ({0}/{1})" -f $inStops, $cycles) ($inStops -eq $cycles)
            Check 'every step-out used AG_GOTILADR with the LR return address' `
                  ((Count-Regex $ev ('AG_GOTILADR\) nSteps=0 adr=0x{0:X8} start' -f $cr)) -eq $cycles)
            Check 'temporary BP cleanup for both runs of every cycle (2 -> 0)' `
                  (((Count-Regex $ev 'temporary breakpoint 0x[0-9A-Fa-f]+ installed \(backend=1\)') -eq (2 * $cycles)) -and
                   ((Count-Regex $ev 'temporary breakpoint 0x[0-9A-Fa-f]+ removed \(backend=0\)') -eq (2 * $cycles)))
            Check 'no connection loss / timeout' `
                  (((Count-Marker $ev 'connection lost') -eq 0) -and ($ses.Exit -eq 0))
        }

        'temp-bp-refcount' {
            # spec 47/48: a USER breakpoint at the same address as the step-out
            # temporary breakpoint -- the refcount must survive and the user
            # breakpoint must not be deleted by the step.
            Set-SourceVariant 'A'
            $b = Invoke-KeilBuild 'refcount' $outDir
            Check 'build: 0 errors' ($b.Errors -eq 0)
            Assert-Build 'refcount' $b
            $fooAddr = Get-MapSymbol $mapFile 'foo'
            $mainAddr = Get-MapSymbol $mapFile 'main'
            $aoAddr = Get-MapSymbol $mapFile 'add_one'
            $barAddr = Get-MapSymbol $mapFile 'bar'
            $simProc = Start-TestSim $PipeName
            $an = Get-StepAnchors $PipeName 'refcount' $aoAddr $barAddr
            Check 'anchors derived' (($an.CallSite -ne '') -and ($an.CallReturn -ne ''))
            $cs = [Convert]::ToUInt32($an.CallSite, 16)
            $cr = [Convert]::ToUInt32($an.CallReturn, 16)
            $ses = Invoke-KeilSession 'temp-bp-refcount' $true $false @(
                ('BS 0x{0:X8}' -f $cr),      # user execution BP exactly at the return point
                ('G,0x{0:X8}' -f $cs),       # run to the call site
                'T',                         # enter the callee
                'O',                         # step out -> temp BP collides with the user BP
                'G',                         # continue: must stop at the user BP again
                'EXIT') $PipeName
            Check 'session exit=0' ($ses.Exit -eq 0) ("exit=$($ses.Exit)")
            $ev = $ses.Evidence
            Check 'user BP installed at the return point' `
                  ((Count-Regex $ev ('Bp link 0x{0:X8} handle' -f ($cr -band 0xFFFFFFFE))) -ge 1)
            Check 'step-out temp BP shared the address without duplicating (backend stays 1)' `
                  (((Count-Regex $ev ('temporary breakpoint 0x{0:X8} installed \(backend=1\)' -f $cr)) -eq 1) -and
                   ((Count-Regex $ev ('temporary breakpoint 0x{0:X8} removed \(backend=1\)' -f $cr)) -eq 1)) `
                  ("inst=" + (Count-Regex $ev ('temporary breakpoint 0x{0:X8} installed' -f $cr)) +
                   " rem=" + (Count-Regex $ev ('temporary breakpoint 0x{0:X8} removed' -f $cr)))
            Check 'the step did not delete the user breakpoint (continue hits it)' `
                  ((Count-BpStopsAt $ev $cr) -ge 2) ("stops=" + (Count-BpStopsAt $ev $cr))
            Check 'exactly one local BP entry left at teardown' `
                  ((Count-Regex $ev 'Bp session reset: 1 local breakpoint entry') -eq 1)
        }

        'step-combined' {
            # spec 58: 100 combined Into/Over/Out/Run cycles; the temporary
            # breakpoint count must return to zero after every cycle.
            $cycles = if ($Rounds -gt 0) { $Rounds } else { 100 }
            Set-SourceVariant 'A'
            $b = Invoke-KeilBuild 'combined' $outDir
            Check 'build: 0 errors' ($b.Errors -eq 0)
            Assert-Build 'combined' $b
            $fooAddr = Get-MapSymbol $mapFile 'foo'
            $mainAddr = Get-MapSymbol $mapFile 'main'
            $aoAddr = Get-MapSymbol $mapFile 'add_one'
            $barAddr = Get-MapSymbol $mapFile 'bar'
            $simProc = Start-TestSim $PipeName
            $an = Get-StepAnchors $PipeName 'combined' $aoAddr $barAddr
            Check 'anchors derived' (($an.CallSite -ne '') -and ($an.CallReturn -ne ''))
            $cs = [Convert]::ToUInt32($an.CallSite, 16)
            $cr = [Convert]::ToUInt32($an.CallReturn, 16)
            $cmds = @()
            for ($i = 0; $i -lt $cycles; $i++) {
                $cmds += ('G,0x{0:X8}' -f $cs)   # run (temp BP)
                $cmds += 'T'                     # step into
                $cmds += 'O'                     # step out
            }
            $cmds += 'EXIT'
            $ses = Invoke-KeilSession 'step-combined' $true $false $cmds $PipeName
            Check 'session exit=0' ($ses.Exit -eq 0) ("exit=$($ses.Exit) ($($ses.Sec)s)")
            $ev = $ses.Evidence
            $into = Count-StepStopsInRange $ev $aoAddr $barAddr
            $inStops = Count-BpStopsAt $ev $cs
            $outStops = Count-BpStopsAt $ev $cr
            Check ("combined stress: {0} cycles Into+Out+Run completed" -f $cycles) `
                  (($into -eq $cycles) -and ($inStops -eq $cycles) -and ($outStops -eq $cycles)) `
                  ("into=$into runToCall=$inStops outStops=$outStops cycles=$cycles")
            Check 'temporary BP count returned to zero after every cycle' `
                  (((Count-Regex $ev 'temporary breakpoint 0x[0-9A-Fa-f]+ installed \(backend=1\)') -eq (2 * $cycles)) -and
                   ((Count-Regex $ev 'temporary breakpoint 0x[0-9A-Fa-f]+ removed \(backend=0\)') -eq (2 * $cycles)) -and
                   ((Count-Regex $ev 'removed \(backend=1\)') -eq 0)) `
                  ("inst=" + (Count-Regex $ev 'installed \(backend=1\)') +
                   " rem0=" + (Count-Regex $ev 'removed \(backend=0\)'))
            Check 'no ghost stop and no connection loss' `
                  (((Count-Marker $ev 'connection lost') -eq 0) -and ($ses.Exit -eq 0))
        }

        'step-runtime' {
            # spec 66/67: interruption of a BLOCKED step/run wait.
            # A second-client HALT is impossible BY DESIGN: the IPC server is
            # single-instance and a second debugger gets "Busy" (spec 46), so
            # the interrupt paths verified here are
            #   (a) the run is NOT bounded by the 2 s RPC timeout -- it must
            #       stay blocked far beyond it and only end on the interruption;
            #   (b) killing the simulator wakes the wait immediately (connection
            #       loss) without any deadlock, and the session ends cleanly.
            # The Stop-button path (AG_STOPRUN from µVision's own thread) uses
            # the same run wait and is covered by the B.3 probe suite
            # (1000x concurrent Run/Halt, 20/20 rounds).
            Set-SourceVariant 'A'
            $b = Invoke-KeilBuild 'step-runtime' $outDir
            Check 'build: 0 errors' ($b.Errors -eq 0)
            Assert-Build 'step-runtime' $b

            $simProc = Start-TestSim $PipeName
            $as = Start-KeilSessionAsync 'step-lost' $true $false @(
                ('G,0x{0:X8}' -f 0x08010000),   # never-reached temp BP -> long run
                'EXIT') $PipeName
            Start-Sleep -Milliseconds 6500       # > 2 s RPC timeout, < session timeout
            Stop-Process -Id $simProc.Id -Force -ErrorAction SilentlyContinue
            $ses = Complete-KeilSessionAsync $as 30
            Check 'session ended after the kill (no hang / no deadlock)' ($ses.Exit -ne 'TIMEOUT') `
                  ("exit=$($ses.Exit)")
            $ev = $ses.Evidence
            Check 'the blocked run woke up on the connection loss' `
                  ((Count-Marker $ev 'connection lost') -ge 1)
            Check 'Keil did not crash' `
                  (($ses.Exit -eq 0) -or ([int]$ses.Exit -ge 0)) ("exit=$($ses.Exit)")
            Check 'the temporary breakpoint of the blocked run was cleaned up' `
                  (((Count-Regex $ev 'temporary breakpoint 0x[0-9A-Fa-f]+ installed \(backend=1\)') -eq 1) -and
                   ((Count-Regex $ev 'temporary breakpoint 0x[0-9A-Fa-f]+ removed \(backend=0\)') -eq 1)) `
                  ("inst=" + (Count-Regex $ev 'installed \(backend=1\)') +
                   " rem0=" + (Count-Regex $ev 'removed \(backend=0\)'))
            # the run stayed blocked far beyond the 2 s RPC timeout
            $startTs = ''; $lostTs = ''
            foreach ($l in (Get-Content -LiteralPath $ev)) {
                if ($startTs -eq '' -and $l -match '^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+) .*AG_GOTILADR\) nSteps=0 .* start') {
                    $startTs = $Matches[1]
                }
                if ($lostTs -eq '' -and $l -match '^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+) .*connection lost') {
                    $lostTs = $Matches[1]
                }
            }
            $blockedSec = if ($startTs -and $lostTs) { ([datetime]$lostTs - [datetime]$startTs).TotalSeconds } else { -1 }
            Check 'the run was NOT bounded by the 2 s RPC timeout (blocked > 2.5 s)' `
                  ($blockedSec -ge 2.5) ("blocked=" + [Math]::Round($blockedSec, 1) + "s")
        }

        'lifecycle-stress' {
            # spec 90: N full source-debug sessions: LoadApp -> run-to-main ->
            # source BP -> run -> step -> exit.  0 crashes / timeouts / stale
            # simulators.
            $rounds = if ($Rounds -gt 0) { $Rounds } else { 30 }
            Set-SourceVariant 'A'
            $b = Invoke-KeilBuild 'lifecycle' $outDir
            Check 'build: 0 errors' ($b.Errors -eq 0)
            Assert-Build 'lifecycle' $b
            $fooAddr = Get-MapSymbol $mapFile 'foo'
            $simProc = Start-TestSim $PipeName
            $ok = 0; $bad = @(); $commits = 0
            for ($i = 1; $i -le $rounds; $i++) {
                $tag = "life-{0:d2}" -f $i
                $ses = Invoke-KeilSession $tag $true $true @(
                    'BS foo', 'G', 'T', 'T', 'O', 'EXIT') $PipeName
                $ev = $ses.Evidence
                $pc = First-Match $ev 'stopped: Breakpoint pc=0x([0-9A-Fa-f]+)'
                $good = ($ses.Exit -eq 0) -and ($pc -ne '') -and
                        (([Convert]::ToUInt32($pc, 16) -band 0xFFFFFFFE) -eq ($fooAddr -band 0xFFFFFFFE)) -and
                        ((Count-Marker $ev '[PROGRAM] end success') -eq 1) -and
                        ((Count-Marker $ev 'connection lost') -eq 0)
                if ($good) { $ok++ } else { $bad += ("#{0}: exit={1} pc=0x{2}" -f $i, $ses.Exit, $pc) }
                $commits += (Count-Marker $ev '[PROGRAM] end success')
            }
            Check ("lifecycle stress: {0}/{1} full sessions clean (LoadApp+run-to-main+BP+steps)" -f $ok, $rounds) `
                  ($ok -eq $rounds) ($bad -join '; ')
            Check 'every round downloaded (LoadApp commit)' ($commits -eq $rounds) ("commits=$commits")
        }
        }
        } catch {
            Check ("scenario $s completed without exception") $false $_.Exception.Message
        }

        $bad = @($script:checks | Where-Object { -not $_.Ok }).Count
        $rows += [pscustomobject]@{ Scenario = $s; Checks = $script:checks.Count; Failed = $bad }
        Write-Host ("    -> {0}/{1} checks passed" -f ($script:checks.Count - $bad), $script:checks.Count) `
                   -ForegroundColor $(if ($bad -eq 0) { 'Green' } else { 'Red' })
        $script:checks | ForEach-Object {
            $status = if ($_.Ok) { 'ok  ' } else { 'FAIL' }
            Add-Content -LiteralPath (Join-Path $outDir ("{0}_checks.txt" -f $s)) `
                        -Value ("{0} {1} {2}" -f $status, $_.Name, $_.Detail)
        }
    }
} finally {
    Copy-Item -LiteralPath $uvoptxBackup -Destination $uvoptx -Force
    Copy-Item -LiteralPath $uvprojxBackup -Destination $uvprojx -Force
    Copy-Item -LiteralPath $mainBackup -Destination $mainC -Force
    Copy-Item -LiteralPath $initBackup -Destination $initFile -Force
    Remove-Item -LiteralPath $uvoptxBackup -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $uvprojxBackup -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $mainBackup -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $initBackup -Force -ErrorAction SilentlyContinue
    Stop-TestUv4
    if ($simProc -and -not $KeepSimulator) { Stop-Process -Id $simProc.Id -Force -ErrorAction SilentlyContinue }
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
$failed = ($rows | Measure-Object -Property Failed -Sum).Sum
$total = ($rows | Measure-Object -Property Checks -Sum).Sum
Write-Host ("result: {0}/{1} checks passed" -f ($total - $failed), $total) `
           -ForegroundColor $(if ($failed -eq 0) { 'Green' } else { 'Red' })
Write-Host ("evidence: {0}" -f $outDir)
exit $(if ($failed -eq 0) { 0 } else { 1 })