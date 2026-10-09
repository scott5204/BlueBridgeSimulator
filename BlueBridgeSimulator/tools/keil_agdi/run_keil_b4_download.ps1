# BlueBridge AGDI -- B.4.2 real application-download acceptance
# (stage 7-2B.4 checkpoint B.4.2, spec sections 43-87).
#
# Drives REAL µVision 5.43 sessions with "Load Application at Startup" enabled
# against a fresh BlueBridgeSimulator and proves that Keil's own download is
# what writes the virtual flash:
#
#   * every download goes through AG_INITSTARTLOAD -> AG_WROPC -> PROGRAM_*
#     (never through the ordinary WRITE_MEMORY path, never preloaded);
#   * the flash bytes are compared record-by-record against the HEX file µVision
#     itself produced in the same build (100% identical = PASS);
#   * A -> B happens in place on ONE simulator: build_magic changes, the
#     same-address `build_variant()` opcode changes, and the CPU then really
#     executes the B opcode (g_variant read back over IPC = no stale TCG);
#   * NOCODE symbol loads touch nothing; a simulator FROZEN mid-transaction
#     makes the driver abort the load without committing (no production test
#     hook: B.4.4 removed ProgramFaultAfterBlock, spec sections 18/19) and the
#     same simulator then recovers and commits the next image; multi-block
#     images work, RAM sections
#     are measured (never guessed) and the µVision AG_RESET after the commit is
#     safe (B.3 event-sequence semantics must survive the double reset);
#   * the ordinary memory-write path (µVision ENTER command = Memory Window) is
#     still refused for flash and still works for RAM -- the application-load
#     path must never leak into generic writes (spec section 61).
#
# Every temporary change (uvoptx tLdApp/tGomain/tIfile, agdi.ini, main.c source
# variant, initialization file) is made under a backup + try/finally restore
# (spec sections 81). Process management only ever touches instances this script
# started (or stale UV4/simulator instances running THIS project/pipe, spec
# 82/83) -- never the user's other Keil windows or simulators.
#
# Usage (one scenario per call; no argument = all):
#   powershell -File tools\keil_agdi\run_keil_b4_download.ps1
#   powershell -File tools\keil_agdi\run_keil_b4_download.ps1 -Scenarios download-ab
#
# MUST run outside the agent sandbox (µVision writes per-user settings, the
# driver writes %LOCALAPPDATA%\BlueBridgeSimulator\logs\BlueBridgeAGDI.log).

[CmdletBinding()]
param(
    [string[]]$Scenarios = @(),
    [int]$TimeoutSec = 40,
    [string]$PipeName = 'BlueBridgeSimulator.Debug.B4DL',
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
$sctRam    = Join-Path $testDir 'b4_sram_init.sct'
$mapFile   = Join-Path $testDir 'bb_test.map'
$hexFile   = Join-Path $testDir 'UVBuild\bb_test.hex'
$outDir    = Join-Path $scriptDir 'build\b4_dl'
$probe     = Join-Path $scriptDir 'build\Release\BlueBridgeAGDIProbe.exe'
$uv4       = Join-Path $env:LOCALAPPDATA 'Keil_v5\UV4\UV4.exe'
$agdiLog   = Join-Path $env:LOCALAPPDATA 'BlueBridgeSimulator\logs\BlueBridgeAGDI.log'
$agdiIni   = Join-Path $env:LOCALAPPDATA 'BlueBridgeSimulator\agdi.ini'

$flashBase = 0x08000000
$flashSize = 0x00020000        # 128 KiB (compared area; caps come from the IPC)

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

$allScenarios = @('download-a', 'download-b', 'download-ab', 'nocode', 'memwrite',
                  'abort', 'multi-block', 'ram-section', 'double-reset', 'disconnect')
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

# NtSuspendProcess/NtResumeProcess: the B.4.4 abort scenario freezes the
# simulator mid-transaction from the SCRIPT side (the production driver has no
# fault-injection switch any more, spec sections 18/19).
if (-not ('BBProc' -as [type])) {
    Add-Type @"
using System; using System.Runtime.InteropServices;
public static class BBProc {
  [DllImport("ntdll.dll")] public static extern int NtSuspendProcess(IntPtr h);
  [DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr h);
  [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
}
"@
}
function Suspend-TestSim([int]$ProcessId) {
    $h = [BBProc]::OpenProcess(0x0800, $false, $ProcessId)   # PROCESS_SUSPEND_RESUME
    if ($h -eq [IntPtr]::Zero) { return $false }
    $r = [BBProc]::NtSuspendProcess($h)
    [BBProc]::CloseHandle($h) | Out-Null
    return ($r -eq 0)
}
function Resume-TestSim([int]$ProcessId) {
    $h = [BBProc]::OpenProcess(0x0800, $false, $ProcessId)
    if ($h -eq [IntPtr]::Zero) { return $false }
    $r = [BBProc]::NtResumeProcess($h)
    [BBProc]::CloseHandle($h) | Out-Null
    return ($r -eq 0)
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

# ---- Intel HEX ----------------------------------------------------------
function Read-HexBytes([string]$Path) {
    $map = @{}
    $upper = 0
    foreach ($l in [System.IO.File]::ReadAllLines($Path)) {
        if ($l.Length -lt 11 -or $l[0] -ne ':') { continue }
        $len  = [Convert]::ToInt32($l.Substring(1, 2), 16)
        $addr = [Convert]::ToInt32($l.Substring(3, 4), 16)
        $type = [Convert]::ToInt32($l.Substring(7, 2), 16)
        $sum  = $len + (($addr -shr 8) -band 0xFF) + ($addr -band 0xFF) + $type
        $data = New-Object byte[] $len
        for ($i = 0; $i -lt $len; $i++) {
            $b = [Convert]::ToInt32($l.Substring(9 + $i * 2, 2), 16)
            $data[$i] = [byte]$b
            $sum += $b
        }
        $ck = [Convert]::ToInt32($l.Substring(9 + $len * 2, 2), 16)
        if ((($sum + $ck) -band 0xFF) -ne 0) { throw "bad HEX checksum in $Path : $l" }
        if ($type -eq 0) {
            for ($i = 0; $i -lt $len; $i++) { $map[$upper + $addr + $i] = $data[$i] }
        } elseif ($type -eq 4) {
            $upper = [Convert]::ToInt32($l.Substring(9, 4), 16) * 0x10000
        } elseif ($type -eq 1) {
            break
        }
    }
    return $map
}

function Get-FlashBytes([string]$Pipe) {
    $out = New-Object System.Collections.Generic.List[byte]
    $done = 0
    while ($done -lt $flashSize) {
        $chunk = [Math]::Min(0x10000, $flashSize - $done)
        $line = & $probe --pipe $Pipe --read ('0x{0:X8}' -f ($flashBase + $done)) $chunk 2>&1 |
                Where-Object { $_ -like 'read:*' } | Select-Object -First 1
        if (-not $line -or $line -notmatch 'data=([0-9a-f]*)') {
            throw "flash read failed at 0x$('{0:X8}' -f ($flashBase + $done)): $line"
        }
        $hex = $Matches[1]
        for ($i = 0; $i -lt $hex.Length; $i += 2) {
            $out.Add([Convert]::ToByte($hex.Substring($i, 2), 16))
        }
        $done += $chunk
    }
    return ,$out.ToArray()
}

function Get-FlashHash([byte[]]$Bytes) {
    $sha = New-Object System.Security.Cryptography.SHA256Managed
    return ([System.BitConverter]::ToString($sha.ComputeHash($Bytes)) -replace '-', '').Substring(0, 16)
}

function Compare-FlashWithHex([byte[]]$Flash, $HexMap) {
    $bad = 0; $n = 0; $firstBad = -1; $outside = 0
    foreach ($a in ($HexMap.Keys | Sort-Object)) {
        if ($a -lt $flashBase -or $a -ge ($flashBase + $flashSize)) {
            $outside++          # e.g. a RAM load region: not flash, not compared
            continue
        }
        $off = $a - $flashBase
        $n++
        if ($Flash[$off] -ne $HexMap[$a]) {
            if ($firstBad -lt 0) { $firstBad = $a }
            $bad++
        }
    }
    return [pscustomobject]@{ Bytes = $n; Bad = $bad; FirstBad = $firstBad; Outside = $outside }
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

# ---- source variant / fixture patches (always under the main.c backup) ---
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

function Add-MainCInsert([string]$Snippet, [string]$ReplaceSrc, [string]$ReplaceDst) {
    $t = [System.IO.File]::ReadAllText($mainC)
    if ($ReplaceSrc -and $t.Contains($ReplaceSrc)) {
        $t = $t.Replace($ReplaceSrc, $ReplaceDst)
    }
    $t = $t.Replace('#include <stdint.h>', "#include <stdint.h>`r`n`r`n$Snippet")
    [System.IO.File]::WriteAllText($mainC, $t, (New-Object System.Text.UTF8Encoding($false)))
}

function Set-RamScatterLayout {
    # a real loadable RAM section (spec section 62): a second load region whose
    # load address is inside SRAM -- the initializer must be programmed at
    # 0x20007000, which is exactly the "does uVision ever WROPC to RAM?" test
    $sct = @'
LR_IROM1 0x08000000 0x00020000  {
  ER_IROM1 0x08000000 0x00020000  {
   *.o (RESET, +First)
   *(InRoot$$Sections)
   .ANY (+RO)
   .ANY (+XO)
  }
  RW_IRAM1 0x20000000 0x00007000  {
   .ANY (+RW +ZI)
  }
}

LR_SRAM_INIT 0x20007000 0x00001000  {
  ER_SRAM_INIT 0x20007000 0x00001000  {
   *(.sram_init)
  }
}
'@
    [System.IO.File]::WriteAllText($sctRam, $sct, (New-Object System.Text.ASCIIEncoding))
    $t = [System.IO.File]::ReadAllText($uvprojx)
    $t = [regex]::Replace($t, '<umfTarg>[01]</umfTarg>', '<umfTarg>0</umfTarg>')
    $t = [regex]::Replace($t, '<ScatterFile>[^<]*</ScatterFile>',
                          '<ScatterFile>.\b4_sram_init.sct</ScatterFile>')
    [System.IO.File]::WriteAllText($uvprojx, $t, (New-Object System.Text.UTF8Encoding($false)))
}

function Assert-Build([string]$tag, $Build) {
    if ($Build.Errors -ne 0) {
        throw "build $tag failed ($($Build.Errors) error(s), see build_$tag.txt)"
    }
}

# ---- session runner -----------------------------------------------------
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

function Invoke-KeilSession([string]$tag, [bool]$LdApp, [bool]$Gomain, [string[]]$Init, [string]$Pipe) {
    Set-UvoptxOptions $uvoptx $LdApp $Gomain '.\exit_init.ini'
    $text = ($Init -join "`r`n")
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
    return [pscustomobject]@{
        Exit = if ($finished) { $pr.ExitCode } else { 'TIMEOUT' }
        Sec = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
        Evidence = $evidence
        Pipe = $Pipe
    }
}

function Count-Marker([string]$Path, [string]$Pattern) {
    if (-not (Test-Path $Path)) { return 0 }
    return (Select-String -Path $Path -Pattern $Pattern -SimpleMatch -ErrorAction SilentlyContinue |
            Measure-Object).Count
}

# ==========================================================================
# environment: backup + patch agdi.ini (restored in the outer finally)
# ==========================================================================
Stop-TestUv4
Stop-TestSim $PipeName

$iniExisted = Test-Path $agdiIni
$iniBackup = "$agdiIni.b4dl.bak"
if ($iniExisted) { Copy-Item -LiteralPath $agdiIni -Destination $iniBackup -Force }
$lines = @()
if ($iniExisted) { $lines = @(Get-Content -LiteralPath $agdiIni) }
if (-not ($lines | Where-Object { $_ -match '^\s*\[BlueBridge\]' })) {
    $lines = @('[BlueBridge]') + $lines
}
$sawAuto = $false; $sawPipe = $false; $sawTrace = $false; $sawLoad = $false
$patched = foreach ($l in $lines) {
    if     ($l -match '^\s*AutoStart\s*=')             { $sawAuto = $true; 'AutoStart=0' }
    elseif ($l -match '^\s*PipeName\s*=')              { $sawPipe = $true; "PipeName=$PipeName" }
    elseif ($l -match '^\s*Trace\s*=')                 { $sawTrace = $true; 'Trace=1' }
    elseif ($l -match '^\s*LoadTrace\s*=')             { $sawLoad = $true; 'LoadTrace=0' }
    elseif ($l -match '^\s*ProgramFaultAfterBlock\s*=') { continue }   # removed from production
    else                                                { $l }
}
if (-not $sawAuto)  { $patched += 'AutoStart=0' }
if (-not $sawPipe)  { $patched += "PipeName=$PipeName" }
if (-not $sawTrace) { $patched += 'Trace=1' }
# B.4.1's verbatim LoadTrace stays OFF here: the [PROGRAM]/run/breakpoint lines
# this acceptance needs are always written, and a verbatim trace of a 61 KiB
# 16-block image would rotate the 8 MiB log mid-session (spec section 78)
if (-not $sawLoad)  { $patched += 'LoadTrace=0' }
[System.IO.File]::WriteAllLines($agdiIni, $patched, (New-Object System.Text.UTF8Encoding($false)))

$uvoptxBackup = "$uvoptx.b4dl.bak"
$uvprojxBackup = "$uvprojx.b4dl.bak"
$mainBackup   = "$mainC.b4dl.bak"
$initBackup   = "$initFile.b4dl.bak"
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

        'download-a' {
            Set-SourceVariant 'A'
            $b = Invoke-KeilBuild 'A' $outDir
            Check 'build A: 0 errors' ($b.Errors -eq 0)
            $hexA = Read-HexBytes $hexFile
            $simProc = Start-TestSim $PipeName
            $before = Get-FlashBytes $PipeName
            Check 'fresh flash is 0xFF before download' `
                  (($before | Where-Object { $_ -ne 0xFF }).Count -eq 0) `
                  ('hash=' + (Get-FlashHash $before))
            $ses = Invoke-KeilSession 'download-a' $true $false @('EXIT') $PipeName
            Check 'session exit=0' ($ses.Exit -eq 0) ("exit=$($ses.Exit) ($($ses.Sec)s)")
            $ev = $ses.Evidence
            Check 'STARTLOAD observed'        ((Count-Marker $ev 'AG_INITSTARTLOAD') -eq 1)
            Check 'ENDLOAD observed'          ((Count-Marker $ev 'AG_INITENDLOAD') -eq 1)
            Check 'PROGRAM_BEGIN exactly once' ((Count-Marker $ev '[PROGRAM] begin') -eq 1)
            Check 'AG_WROPC flash mapping'     ((Count-Marker $ev 'wropc block 1: 0x08000000') -eq 1)
            Check 'PROGRAM_ERASE'              ((Count-Marker $ev '[PROGRAM] erase 0x08000000') -ge 1)
            Check 'PROGRAM_END exactly once'   ((Count-Marker $ev '[PROGRAM] end success') -eq 1)
            Check 'vector verify match'        ((Count-Marker $ev 'match=1') -ge 1)
            $after = Get-FlashBytes $PipeName
            $cmp = Compare-FlashWithHex $after $hexA
            Check 'flash readback == HEX (A)'  ($cmp.Bad -eq 0) `
                  ("bytes=$($cmp.Bytes) bad=$($cmp.Bad)")
            Check 'flash really changed'       ((Get-FlashHash $after) -ne (Get-FlashHash $before)) `
                  ('before=' + (Get-FlashHash $before) + ' after=' + (Get-FlashHash $after))
            Check 'alias mirrors flash'        ((Read-Mem32 $PipeName 0x00000000) -eq (Read-Mem32 $PipeName 0x08000000))
        }

        'download-b' {
            Set-SourceVariant 'B'
            $b = Invoke-KeilBuild 'B' $outDir
            Check 'build B: 0 errors' ($b.Errors -eq 0)
            $hexB = Read-HexBytes $hexFile
            $simProc = Start-TestSim $PipeName
            $ses = Invoke-KeilSession 'download-b' $true $false @('EXIT') $PipeName
            Check 'session exit=0' ($ses.Exit -eq 0) ("exit=$($ses.Exit)")
            $after = Get-FlashBytes $PipeName
            $cmp = Compare-FlashWithHex $after $hexB
            Check 'flash readback == HEX (B)' ($cmp.Bad -eq 0) ("bytes=$($cmp.Bytes) bad=$($cmp.Bad)")
        }

        'download-ab' {
            # A first (own build), run A, then build B and download INTO THE SAME
            # simulator -- the in-place A->B the spec asks for (46-53)
            Set-SourceVariant 'A'
            $b = Invoke-KeilBuild 'AB-A' $outDir
            Check 'build A: 0 errors' ($b.Errors -eq 0)
            $hexA = Read-HexBytes $hexFile
            $varAddrA = Get-MapSymbol $mapFile 'build_variant'
            $gvAddrA  = Get-MapSymbol $mapFile 'g_variant'
            Check 'map: build_variant found' ($null -ne $varAddrA) ('0x{0:X8}' -f $varAddrA)

            $simProc = Start-TestSim $PipeName
            $sesA1 = Invoke-KeilSession 'ab-a-load' $true $false @('EXIT') $PipeName
            Check 'A download session exit=0' ($sesA1.Exit -eq 0)
            $flashA = Get-FlashBytes $PipeName
            $cmpA = Compare-FlashWithHex $flashA $hexA
            Check 'A flash == HEX' ($cmpA.Bad -eq 0) ("bad=$($cmpA.Bad)")
            $opcodeA = Read-Mem32 $PipeName ($varAddrA -band 0xFFFFFFFE)
            Check 'A opcode read' ($null -ne $opcodeA) ('0x{0:X8} @0x{1:X8}' -f $opcodeA, ($varAddrA -band 0xFFFFFFFE))

            # execute the A image (source BP + run): fills the TCG cache with A
            $sesA2 = Invoke-KeilSession 'ab-a-run' $true $false @('BS bar', 'G', 'EXIT') $PipeName
            Check 'A run session exit=0' ($sesA2.Exit -eq 0)
            Check 'A breakpoint hit' ((Count-Marker $sesA2.Evidence 'stopped: Breakpoint') -ge 1)
            $gvA = Read-Mem32 $PipeName $gvAddrA
            Check 'A executed build_variant -> 0x11' ($gvA -eq 0x11) ('g_variant=0x{0:X8}' -f $gvA)
            $magicA = Read-Mem32 $PipeName (Get-MapSymbol $mapFile 'build_magic')
            Check 'A startup copied magic 0x11111111' ($magicA -eq 0x11111111) ('build_magic=0x{0:X8}' -f $magicA)

            # now B, same simulator, same session pattern
            Set-SourceVariant 'B'
            $b2 = Invoke-KeilBuild 'AB-B' $outDir
            Check 'build B: 0 errors' ($b2.Errors -eq 0)
            $hexB = Read-HexBytes $hexFile
            $varAddrB = Get-MapSymbol $mapFile 'build_variant'
            $gvAddrB  = Get-MapSymbol $mapFile 'g_variant'
            Check 'build_variant address unchanged' ($varAddrA -eq $varAddrB) `
                  ('A=0x{0:X8} B=0x{1:X8}' -f $varAddrA, $varAddrB)
            Check 'g_variant address unchanged' ($gvAddrA -eq $gvAddrB)

            $sesB1 = Invoke-KeilSession 'ab-b-load' $true $false @('EXIT') $PipeName
            Check 'B download session exit=0' ($sesB1.Exit -eq 0)
            $flashB = Get-FlashBytes $PipeName
            $cmpB = Compare-FlashWithHex $flashB $hexB
            Check 'B flash == HEX (in place)' ($cmpB.Bad -eq 0) ("bad=$($cmpB.Bad)")
            $cmpOld = Compare-FlashWithHex $flashB $hexA
            Check 'B flash != A HEX' ($cmpOld.Bad -gt 0) ("differences=$($cmpOld.Bad)")
            $opcodeB = Read-Mem32 $PipeName ($varAddrB -band 0xFFFFFFFE)
            Check 'same-address opcode changed' ($opcodeA -ne $opcodeB) `
                  ('A=0x{0:X8} B=0x{1:X8}' -f $opcodeA, $opcodeB)

            $sesB2 = Invoke-KeilSession 'ab-b-run' $true $false @('BS bar', 'G', 'EXIT') $PipeName
            Check 'B run session exit=0' ($sesB2.Exit -eq 0)
            Check 'B breakpoint hit' ((Count-Marker $sesB2.Evidence 'stopped: Breakpoint') -ge 1)
            $gvB = Read-Mem32 $PipeName $gvAddrB
            Check 'CPU executed B opcode -> 0x22 (no stale TCG)' ($gvB -eq 0x22) ('g_variant=0x{0:X8}' -f $gvB)
            $magicB = Read-Mem32 $PipeName (Get-MapSymbol $mapFile 'build_magic')
            Check 'B startup copied magic 0x22222222' ($magicB -eq 0x22222222) ('build_magic=0x{0:X8}' -f $magicB)
        }

        'nocode' {
            Set-SourceVariant 'A'
            $b = Invoke-KeilBuild 'nocode' $outDir
            Check 'build: 0 errors' ($b.Errors -eq 0)
            $simProc = Start-TestSim $PipeName
            $before = Get-FlashBytes $PipeName
            $ses = Invoke-KeilSession 'nocode' $false $false @('LOAD %L NOCODE INCREMENTAL', 'EXIT') $PipeName
            Check 'session exit=0' ($ses.Exit -eq 0)
            $ev = $ses.Evidence
            Check 'LOADPARMS noCode=1 seen' ((Count-Marker $ev 'noCode=1') -ge 1)
            Check 'no PROGRAM_BEGIN'  ((Count-Marker $ev '[PROGRAM] begin') -eq 0)
            Check 'no PROGRAM_ERASE'  ((Count-Marker $ev '[PROGRAM] erase') -eq 0)
            Check 'no PROGRAM_WRITE'  ((Count-Marker $ev 'wropc block') -eq 0)
            Check 'no PROGRAM_END'    ((Count-Marker $ev '[PROGRAM] end success') -eq 0)
            $after = Get-FlashBytes $PipeName
            Check 'flash untouched by NOCODE' ((Get-FlashHash $after) -eq (Get-FlashHash $before))
        }

        'memwrite' {
            # section 61: the ordinary memory-write path (µVision's ENTER command
            # drives the same AG_MemAcc write as an edit in the Memory Window)
            # must still refuse flash. One real Keil download first -- then a
            # second session WITHOUT a load whose init file edits the flash
            # (must be rejected) and an SRAM cell (must succeed).
            Set-SourceVariant 'A'
            $b = Invoke-KeilBuild 'memwrite' $outDir
            Check 'build: 0 errors' ($b.Errors -eq 0)
            $hex = Read-HexBytes $hexFile
            $simProc = Start-TestSim $PipeName

            $sesLoad = Invoke-KeilSession 'memwrite-load' $true $false @('EXIT') $PipeName
            Check 'download session exit=0' ($sesLoad.Exit -eq 0)
            $flashRef = Get-FlashBytes $PipeName
            Check 'reference image == HEX' ((Compare-FlashWithHex $flashRef $hex).Bad -eq 0)

            $ses = Invoke-KeilSession 'memwrite' $false $false @(
                'E CHAR 0x08000000 = 0x12',
                'E CHAR 0x20000100 = 0xAB,0xCD',
                'EXIT') $PipeName
            Check 'write session exit=0' ($ses.Exit -eq 0) ("exit=$($ses.Exit)")
            $ev = $ses.Evidence
            $flashAttempt = @(Select-String -Path $ev -Pattern 'WRITE AG_\w+ 0x08000000 \+\d+ rejected status=5' -ErrorAction SilentlyContinue).Count
            $flashRo      = @(Select-String -Path $ev -Pattern 'WRITE AG_\w+ 0x08000000 \+\d+ rejected status=5 -> AG_RO' -ErrorAction SilentlyContinue).Count
            $flashOk      = @(Select-String -Path $ev -Pattern 'WRITE AG_\w+ 0x08000000 \+\d+ ok' -ErrorAction SilentlyContinue).Count
            $ramOk        = @(Select-String -Path $ev -Pattern 'WRITE AG_\w+ 0x20000100 \+\d+ ok' -ErrorAction SilentlyContinue).Count
            Check 'uVision attempted the flash write through AGDI' ($flashAttempt -ge 1) ("rejected lines=$flashAttempt")
            Check 'flash write answered read-only (Unsupported=5 -> AG_RO)' ($flashRo -ge 1)
            Check 'no flash write ever succeeded' ($flashOk -eq 0)
            Check 'RAM write through the same path still works' ($ramOk -ge 1) ("write lines=$ramOk")
            $ram = Read-Mem32 $PipeName 0x20000100
            Check 'RAM readback 0x0000CDAB' ($ram -eq 0x0000CDAB) ('0x{0:X8}' -f $ram)
            $after = Get-FlashBytes $PipeName
            $cmp = Compare-FlashWithHex $after $hex
            Check 'flash unchanged (generic write had no effect)' ($cmp.Bad -eq 0) `
                  ("bad=$($cmp.Bad)")
            Check 'flash hash unchanged' ((Get-FlashHash $after) -eq (Get-FlashHash $flashRef))
        }

        'abort' {
            # B.4.4: the old production fault-injection hook is gone (spec 18/19).
            # The scenario freezes the simulator from the SCRIPT side in the
            # middle of the B download (suspended mid-transaction -> every
            # PROGRAM_* RPC times out), proving: the driver aborts the
            # transaction, nothing commits, Keil survives, the live flash keeps
            # the previous image, and the same simulator recovers afterwards.
            Set-SourceVariant 'A'
            $b = Invoke-KeilBuild 'abort-A' $outDir
            Check 'build A: 0 errors' ($b.Errors -eq 0)
            $hexA = Read-HexBytes $hexFile
            $simProc = Start-TestSim $PipeName
            $sesA = Invoke-KeilSession 'abort-a-load' $true $false @('EXIT') $PipeName
            Check 'A downloaded first' ($sesA.Exit -eq 0)
            $flashA = Get-FlashBytes $PipeName
            Check 'A flash == HEX' ((Compare-FlashWithHex $flashA $hexA).Bad -eq 0)

            Set-SourceVariant 'B'
            Add-MainCInsert @'
/* B.4.4 abort fixture: a second RO load region inside the flash */
const uint32_t b4_abort_table[4] __attribute__((section(".ARM.__at_0x0800F000"), used)) =
    { 0xB4B40021u, 0xB4B40022u, 0xB4B40023u, 0xB4B40024u };
'@
            $b2 = Invoke-KeilBuild 'abort-B' $outDir
            Check 'build B: 0 errors' ($b2.Errors -eq 0)
            $hexB = Read-HexBytes $hexFile

            Set-UvoptxOptions $uvoptx $true $false '.\exit_init.ini'
            [System.IO.File]::WriteAllText($initFile, "EXIT`r`n",
                                           (New-Object System.Text.ASCIIEncoding))
            $logLen = 0
            if (Test-Path $agdiLog) { $logLen = (Get-Item $agdiLog).Length }
            $out = Join-Path $outDir 'abort_uv4.txt'
            $pr = Start-Process -FilePath $uv4 -ArgumentList @('-d', $proj, '-j0', '-sg', ('-o' + $out)) `
                                -PassThru -WindowStyle Hidden

            $anchor = $false; $frozen = $false; $failed = $false
            $fsLog = [System.IO.File]::Open($agdiLog, 'Open', 'Read', 'ReadWrite')
            $fsLog.Seek($logLen, 'Begin') | Out-Null
            $srLog = New-Object System.IO.StreamReader($fsLog)
            $buf = ''
            try {
                # 1) freeze the simulator as soon as the transaction is open
                $deadline = (Get-Date).AddSeconds(25)
                while ((Get-Date) -lt $deadline -and -not $pr.HasExited) {
                    $chunk = $srLog.ReadToEnd()
                    if ($chunk) { $buf += $chunk }
                    if ($buf -match 'wropc block 2') { $anchor = $true; break }
                    Start-Sleep -Milliseconds 5
                }
                if ($anchor) { $frozen = Suspend-TestSim $simProc.Id }
                Check 'freeze landed inside the transaction' ($anchor -and $frozen) `
                      ("anchor=$anchor frozen=$frozen")
                # 2) wait for the driver's first failure evidence
                $deadline = (Get-Date).AddSeconds(20)
                while ((Get-Date) -lt $deadline -and -not $pr.HasExited) {
                    $chunk = $srLog.ReadToEnd()
                    if ($chunk) { $buf += $chunk }
                    if ($buf -match 'load session FAILED' -or $buf -match 'failed status=') { $failed = $true; break }
                    Start-Sleep -Milliseconds 20
                }
                Check 'driver aborted the transaction' $failed
            } finally {
                $srLog.Close(); $fsLog.Close()
                if ($frozen) { Resume-TestSim $simProc.Id | Out-Null }   # never leave it frozen
            }
            $finished = $pr.WaitForExit($TimeoutSec * 1000)
            if (-not $finished) { $pr.Kill(); Start-Sleep -Milliseconds 600 }

            $ev = Join-Path $outDir 'abort_agdi.log'
            $delta = Read-LogDelta $agdiLog $logLen
            if ($delta.Length -gt 0) { Set-Content -LiteralPath $ev -Value $delta -Encoding UTF8 }
            $exitCode = if ($finished) { [int]$pr.ExitCode } else { 'TIMEOUT' }
            $crash = ($exitCode -ne 'TIMEOUT') -and ($exitCode -lt -100000)
            Check 'program RPC failed inside the load' ((Count-Marker $ev 'failed status=') -ge 1)
            Check 'no commit in failed load' ((Count-Marker $ev '[PROGRAM] end success') -eq 0)
            Check 'session FAILED logged' ((Count-Marker $ev 'load session FAILED') -ge 1)
            Check 'Keil survived the failure' ((-not $crash) -and ($exitCode -ne 'TIMEOUT')) `
                  ("exit=$exitCode")

            # let the resumed server run its disconnect cleanup, then verify the
            # live flash still holds image A (no partial B)
            $deadline = (Get-Date).AddSeconds(10)
            while ((Get-Date) -lt $deadline -and
                   -not @(Get-ChildItem '\\.\pipe\' -ErrorAction SilentlyContinue |
                          Where-Object { $_.Name -eq $PipeName }).Count) {
                Start-Sleep -Milliseconds 100
            }
            Start-Sleep -Milliseconds 500
            $flashAfter = Get-FlashBytes $PipeName
            Check 'live flash still A (no half image)' ((Compare-FlashWithHex $flashAfter $hexA).Bad -eq 0)
            Check 'live flash is not B yet' ((Compare-FlashWithHex $flashAfter $hexB).Bad -gt 0)

            # recovery: the same simulator accepts a new load and commits B
            $sesR = Invoke-KeilSession 'abort-recovery-b' $true $false @('EXIT') $PipeName
            Check 'recovery session exit=0' ($sesR.Exit -eq 0) ("exit=$($sesR.Exit)")
            $flashR = Get-FlashBytes $PipeName
            Check 'recovery load committed B == HEX' ((Compare-FlashWithHex $flashR $hexB).Bad -eq 0)
        }

        'multi-block' {
            Set-SourceVariant 'A'
            Add-MainCInsert @'
/* B.4.2 multi-block fixture: a second RO load region inside the flash */
const uint32_t b4_extra_table[4] __attribute__((section(".ARM.__at_0x0800F000"), used)) =
    { 0xB4B40001u, 0xB4B40002u, 0xB4B40003u, 0xB4B40004u };
'@
            $b = Invoke-KeilBuild 'multi' $outDir
            Check 'build: 0 errors' ($b.Errors -eq 0) ($b.Log -split "`r?`n" | Select-String 'Program Size' | Select-Object -First 1)
            $hex = Read-HexBytes $hexFile
            $simProc = Start-TestSim $PipeName
            $ses = Invoke-KeilSession 'multi-block' $true $false @('EXIT') $PipeName
            Check 'session exit=0' ($ses.Exit -eq 0)
            $ev = $ses.Evidence
            $blocks = Count-Marker $ev 'wropc block'
            Check 'multiple WROPC blocks (>1)' ($blocks -gt 1) ("blocks=$blocks")
            Check 'PROGRAM_END exactly once' ((Count-Marker $ev '[PROGRAM] end success') -eq 1)
            $secondRegion = @(Select-String -Path $ev -Pattern 'wropc block' -ErrorAction SilentlyContinue |
                              Where-Object { $_.Line -match '0x0800F000' }).Count
            Check 'second region 0x0800F000 written' ($secondRegion -ge 1) ("blocks@F000=$secondRegion")
            $after = Get-FlashBytes $PipeName
            $cmp = Compare-FlashWithHex $after $hex
            Check 'both regions == HEX' ($cmp.Bad -eq 0) ("bytes=$($cmp.Bytes) bad=$($cmp.Bad)")
        }

        'ram-section' {
            Set-SourceVariant 'A'
            Set-RamScatterLayout
            Add-MainCInsert @'
/* B.4.2 RAM load-region fixture: this initializer is programmed at the SRAM
 * load address 0x20007000 (see b4_sram_init.sct), not into the flash */
volatile uint32_t b4_sram_init __attribute__((section(".sram_init"), used)) = 0xA5A5A5A5u;
'@ 'g_counter = build_magic;' 'g_counter = build_magic + b4_sram_init;'
            $b = Invoke-KeilBuild 'ram' $outDir
            Check 'build: 0 errors' ($b.Errors -eq 0)
            Assert-Build 'ram' $b
            $hex = Read-HexBytes $hexFile
            $ramRecords = @($hex.Keys | Where-Object { $_ -ge 0x20007000 -and $_ -lt 0x20008000 }).Count
            Check 'HEX carries a RAM load record' ($ramRecords -gt 0) ("records@0x20007xxx=$ramRecords")
            $simProc = Start-TestSim $PipeName
            $ses = Invoke-KeilSession 'ram-section' $true $false @('EXIT') $PipeName
            Check 'session exit=0' ($ses.Exit -eq 0)
            $ev = $ses.Evidence
            $ramBlock = @(Select-String -Path $ev -Pattern 'wropc block' -ErrorAction SilentlyContinue |
                          Where-Object { $_.Line -match '0x20007000' }).Count
            Check 'uVision sent a RAM WROPC block' ($ramBlock -ge 1) ("blocks@RAM=$ramBlock")
            Check 'RAM routed to WRITE_MEMORY (not staged)' `
                  ((Count-Marker $ev '[PROGRAM] ram write 0x20007000') -ge 1)
            Check 'summary counts RAM bytes separately' `
                  ((Count-Marker $ev 'ram=4B') -ge 1)
            Check 'no PROGRAM_ERASE for RAM' ((Count-Marker $ev '[PROGRAM] erase 0x2000') -eq 0)
            $ram = Read-Mem32 $PipeName 0x20007000
            Check 'RAM write confirmed in-session (readback == bytes)' `
                  ((Count-Marker $ev 'ram readback 0x20007000 +4 == written bytes') -ge 1)
            Check 'post-reset SRAM is zeroed (documented simulator reset)' `
                  ($ram -eq 0) ('0x20007000=0x{0:X8} after the post-load reset' -f $ram)
            $after = Get-FlashBytes $PipeName
            $cmp = Compare-FlashWithHex $after $hex
            Check 'flash part (excl. RAM load region) == HEX' ($cmp.Bad -eq 0) `
                  ("bytes=$($cmp.Bytes) bad=$($cmp.Bad) outsideFlash=$($cmp.Outside)")
        }

        'double-reset' {
            Set-SourceVariant 'A'
            $b = Invoke-KeilBuild 'double-reset' $outDir
            Check 'build: 0 errors' ($b.Errors -eq 0)
            $simProc = Start-TestSim $PipeName
            $ses = Invoke-KeilSession 'double-reset' $true $false @('BS bar', 'G', 'EXIT') $PipeName
            Check 'session exit=0' ($ses.Exit -eq 0)
            $ev = $ses.Evidence
            Check 'PROGRAM_BEGIN exactly once' ((Count-Marker $ev '[PROGRAM] begin') -eq 1)
            Check 'PROGRAM_END exactly once'   ((Count-Marker $ev '[PROGRAM] end success') -eq 1)
            Check 'PROGRAM_END reset ran'      ((Count-Marker $ev 'reset+halt') -ge 1)
            Check 'uVision AG_RESET after load' ((Count-Marker $ev 'AGDI Reset -> IPC RESET_HALT ok') -ge 1)
            Check 'source BP hit after double reset' ((Count-Marker $ev 'stopped: Breakpoint') -ge 1)
            Check 'no connection loss'         ((Count-Marker $ev 'connection lost') -eq 0)
        }

        'disconnect' {
            # spec section 59: kill the simulator IN THE MIDDLE of the program
            # transaction (anchored on a log line of a late 4 KiB block of the
            # multi-block image) -- Keil must not crash and nothing may commit.
            Set-SourceVariant 'A'
            Add-MainCInsert @'
/* B.4.2 disconnect fixture: a second RO load region inside the flash */
const uint32_t b4_extra_table[4] __attribute__((section(".ARM.__at_0x0800F000"), used)) =
    { 0xB4B40001u, 0xB4B40002u, 0xB4B40003u, 0xB4B40004u };
'@
            $b = Invoke-KeilBuild 'disconnect' $outDir
            Check 'build: 0 errors' ($b.Errors -eq 0)
            Assert-Build 'disconnect' $b
            $simProc = Start-TestSim $PipeName

            Set-UvoptxOptions $uvoptx $true $false '.\exit_init.ini'
            [System.IO.File]::WriteAllText($initFile, "EXIT`r`n",
                                           (New-Object System.Text.ASCIIEncoding))
            $logLen = 0
            if (Test-Path $agdiLog) { $logLen = (Get-Item $agdiLog).Length }
            $out = Join-Path $outDir 'disconnect_uv4.txt'
            $pr = Start-Process -FilePath $uv4 -ArgumentList @('-d', $proj, '-j0', '-sg', ('-o' + $out)) `
                                -PassThru -WindowStyle Hidden

            $seen = $false
            $deadline = (Get-Date).AddSeconds(25)
            # incremental read: LoadTrace=1 floods the log, so a plain tail
            # would miss the anchor line of the 4 KiB-block burst
            $fsLog = [System.IO.File]::Open($agdiLog, 'Open', 'Read', 'ReadWrite')
            $fsLog.Seek($logLen, 'Begin') | Out-Null
            $srLog = New-Object System.IO.StreamReader($fsLog)
            $buf = ''
            try {
                while ((Get-Date) -lt $deadline -and -not $pr.HasExited) {
                    $chunk = $srLog.ReadToEnd()
                    if ($chunk) { $buf += $chunk }
                    if ($buf -match 'wropc block 6') { $seen = $true; break }
                    Start-Sleep -Milliseconds 5
                }
            } finally {
                $srLog.Close(); $fsLog.Close()
            }
            if ($seen) { Stop-Process -Id $simProc.Id -Force -ErrorAction SilentlyContinue }
            $finished = $pr.WaitForExit($TimeoutSec * 1000)
            if (-not $finished) { $pr.Kill(); Start-Sleep -Milliseconds 600 }

            $ev = Join-Path $outDir 'disconnect_agdi.log'
            $delta = Read-LogDelta $agdiLog $logLen
            if ($delta.Length -gt 0) { Set-Content -LiteralPath $ev -Value $delta -Encoding UTF8 }
            $exitCode = if ($finished) { [int]$pr.ExitCode } else { 'TIMEOUT' }
            $crash = ($exitCode -ne 'TIMEOUT') -and ($exitCode -lt -100000)
            Check 'kill landed inside the transaction' $seen
            Check 'no commit after the kill' ((Count-Marker $ev '[PROGRAM] end success') -eq 0)
            Check 'driver reported the failed transaction' `
                  (((Count-Marker $ev 'load session FAILED') -ge 1) -or
                   ((Count-Marker $ev 'connection lost') -ge 1))
            Check 'Keil did not crash' (-not $crash) ("exit=$exitCode")

            # a fresh simulator starts from 0xFF again: no half-written image
            # survives anywhere (the transaction died with its simulator)
            $simProc = Start-TestSim $PipeName
            $fresh = Get-FlashBytes $PipeName
            Check 'fresh simulator flash is 0xFF' `
                  (($fresh | Where-Object { $_ -ne 0xFF }).Count -eq 0)
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
    Remove-Item -LiteralPath $sctRam -Force -ErrorAction SilentlyContinue
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