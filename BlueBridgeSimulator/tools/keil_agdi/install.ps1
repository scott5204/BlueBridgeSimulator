# BlueBridgeAGDI install script (stage 7-2B.4.4, spec sections 46-65)
#
# Installs the Keil uVision AGDI driver for the BlueBridge CT117E-M4 virtual
# board. Works both from the RELEASE package layout and from the dev tree:
#
#   release package            dev tree (this repository)
#   ------------------------   -------------------------------------------
#   <pkg>\install.ps1          BlueBridgeSimulator\tools\keil_agdi\install.ps1
#   <pkg>\Keil\BlueBridgeAGDI.dll  ...\tools\keil_agdi\build\Release\BlueBridgeAGDI.dll
#   <pkg>\Simulator\bluesim.exe    BlueBridgeSimulator\build\bluesim.exe
#
# What it does (and does NOT do):
#   * finds the Keil installation (-KeilPath, registry, well-known locations)
#   * verifies UV4.exe exists, prints its version and warns when it is not the
#     version this driver was tested with (5.43.1.0 / MDK 5.43a)
#   * verifies DLL bitness == UV4 bitness (x86 for stock uVision)
#   * asks the user to close uVision (never kills it)
#   * copies ONLY BlueBridge files into <KeilRoot>\ARM\BlueBridgeAGDI\
#     (never patches UV4.exe / SARMCM3.dll / compilers / pack binaries)
#   * adds / updates ONE TDRV slot in [ARM] and [ARMADS] and its entry in the
#     Cortex-M CPUDLL1 group - smallest free slot, idempotent, upgrade-safe;
#     TOOLS.INI is edited in place (never overwritten from a template) and
#     backed up only when it really changes
#   * writes %LOCALAPPDATA%\BlueBridgeSimulator\agdi.ini with the ABSOLUTE
#     SimulatorPath (re-run after moving the product folder to repair it)
#
# Examples:
#   powershell -File install.ps1
#   powershell -File install.ps1 -KeilPath "C:\Keil_v5" -SimulatorPath "D:\Tools\BlueBridge\Simulator\bluesim.exe"

[CmdletBinding()]
param(
    [string]$DllPath = "",
    [Alias('KeilRoot')][string]$KeilPath = "",
    [string]$DisplayName = "BlueBridge CT117E-M4 Simulator",
    [string]$SimulatorPath = "",
    [switch]$SkipAgdiIni
)

$ErrorActionPreference = 'Stop'

$here    = Split-Path -Parent $MyInvocation.MyCommand.Path
$agdiDir = Join-Path $here 'BlueBridgeAGDI'          # only in the dev tree
$simRoot = Split-Path -Parent (Split-Path -Parent $here)   # BlueBridgeSimulator (dev tree)

# --------------------------------------------------------------------------
# 0. resolve the files that belong to this product
# --------------------------------------------------------------------------
if (-not $DllPath) {
    $candidates = @(
        (Join-Path $here 'Keil\BlueBridgeAGDI.dll'),                  # release package
        (Join-Path $here 'build\Release\BlueBridgeAGDI.dll'),         # dev tree
        (Join-Path $here 'build\Debug\BlueBridgeAGDI.dll')
    )
    foreach ($c in $candidates) { if (Test-Path $c) { $DllPath = $c; break } }
}
if (-not $DllPath -or -not (Test-Path $DllPath)) {
    throw "BlueBridgeAGDI.dll not found - run the release package or build_agdi.ps1 first (looked near '$here')."
}
$DllPath = (Resolve-Path -LiteralPath $DllPath).Path

if (-not $SimulatorPath) {
    $candidates = @(
        (Join-Path $here 'Simulator\bluesim.exe'),                    # release package
        (Join-Path $simRoot 'build\bluesim.exe'),                     # dev tree
        (Join-Path $simRoot 'release\bluesim.exe')
    )
    foreach ($c in $candidates) { if ($c -and (Test-Path $c)) { $SimulatorPath = $c; break } }
}

function Get-PeMachine([string]$Path) {
    if (-not (Test-Path $Path)) { return 0 }
    $fs = [System.IO.File]::OpenRead($Path)
    try {
        $br = New-Object System.IO.BinaryReader($fs)
        $fs.Position = 0x3C
        $peOff = $br.ReadInt32()
        $fs.Position = $peOff + 4
        return [int]$br.ReadUInt16()
    } finally { $fs.Close() }
}

# --------------------------------------------------------------------------
# 1. locate Keil
# --------------------------------------------------------------------------
function Find-Keil([string]$Hint) {
    $probe = New-Object System.Collections.Generic.List[string]
    if ($Hint) { $probe.Add($Hint) }

    # registry (regular MDK installation record)
    foreach ($key in @(
        'HKCU:\SOFTWARE\Keil\Products\MDK',
        'HKLM:\SOFTWARE\WOW6432Node\Keil\Products\MDK',
        'HKLM:\SOFTWARE\Keil\Products\MDK')) {
        try {
            $p = (Get-ItemProperty -Path $key -ErrorAction Stop).Path
            if ($p) { $probe.Add($p) }
        } catch { }
    }

    # well-known locations (generic names, not a machine-specific path)
    $probe.Add((Join-Path $env:LOCALAPPDATA 'Keil_v5'))
    $probe.Add('C:\Keil_v5')
    if ($env:Keil_PATH) { $probe.Add($env:Keil_PATH) }

    foreach ($p in $probe) {
        if (-not $p) { continue }
        $p = $p.TrimEnd('\')
        if ((Test-Path (Join-Path $p 'UV4\UV4.exe')) -and (Test-Path (Join-Path $p 'TOOLS.INI'))) {
            return $p
        }
    }
    return ''
}

if (-not $KeilPath) { $KeilPath = Find-Keil '' }
if (-not $KeilPath) {
    throw "Keil installation not found. Pass it explicitly: -KeilPath <folder that contains UV4\UV4.exe>."
}
$KeilPath = (Resolve-Path -LiteralPath $KeilPath).Path
$uv4    = Join-Path $KeilPath 'UV4\UV4.exe'
$ini    = Join-Path $KeilPath 'TOOLS.INI'
$armDir = Join-Path $KeilPath 'ARM'
if (-not (Test-Path $uv4)) { throw "UV4.exe not found at $uv4" }
if (-not (Test-Path $ini)) { throw "TOOLS.INI not found at $ini" }

# --------------------------------------------------------------------------
# 2. checks: version, bitness, uVision not running
# --------------------------------------------------------------------------
$uv4Ver    = (Get-Item $uv4).VersionInfo.FileVersion
$uv4Machine = Get-PeMachine $uv4
$dllMachine = Get-PeMachine $DllPath
if ($uv4Machine -ne $dllMachine) {
    throw ("bitness mismatch: UV4 0x{0:X4} vs DLL 0x{1:X4} - wrong package for this uVision." -f $uv4Machine, $dllMachine)
}

Write-Host "BlueBridge Simulator - AGDI driver install" -ForegroundColor Cyan
Write-Host ("  Keil root : {0}" -f $KeilPath)
Write-Host ("  uVision   : {0} (file version {1})" -f $uv4, $uv4Ver)
Write-Host ("  DLL       : {0} (PE machine 0x{1:X4} - matches uVision)" -f $DllPath, $dllMachine)
if ($uv4Ver -notlike '5.43*') {
    Write-Warning ("This driver was tested with uVision 5.43.1.0 (MDK 5.43a); {0} is untested. Installing anyway." -f $uv4Ver)
}
if ($SimulatorPath) {
    Write-Host ("  Simulator : {0}" -f $SimulatorPath)
} else {
    Write-Warning "bluesim.exe not found near this script - agdi.ini will not get a SimulatorPath (pass -SimulatorPath once you know it)."
}

$uv4Running = @(Get-Process -Name 'UV4' -ErrorAction SilentlyContinue).Count
if ($uv4Running -gt 0) {
    Write-Warning "uVision is running. Close it and start it again after the install, otherwise the new driver entry is not visible."
}

# --------------------------------------------------------------------------
# 3. copy the driver into its own Keil ARM sub-folder
# --------------------------------------------------------------------------
$targetDir = Join-Path $armDir 'BlueBridgeAGDI'
try {
    New-Item -ItemType Directory -Path $targetDir -Force | Out-Null
    Copy-Item $DllPath -Destination $targetDir -Force
} catch [System.UnauthorizedAccessException] {
    throw "Access denied writing '$targetDir'. Please run PowerShell as Administrator (or install Keil for the current user) and retry."
} catch {
    if ($_.Exception.Message -match 'Access|denied') {
        throw "Access denied writing '$targetDir'. Please run PowerShell as Administrator and retry. ($($_.Exception.Message))"
    }
    throw
}
$relPath = 'BlueBridgeAGDI\BlueBridgeAGDI.dll'
Write-Host ("  installed : {0}" -f (Join-Path $targetDir 'BlueBridgeAGDI.dll')) -ForegroundColor Green

# --------------------------------------------------------------------------
# 4. patch TOOLS.INI: one TDRV slot in [ARM] + [ARMADS] + the Cortex-M group
#    (in-place edit of the real file; the whole file is never replaced)
# --------------------------------------------------------------------------
$lines = @(Get-Content -LiteralPath $ini)
$sectionRe = '^\s*\[(.+?)\]\s*$'
$tdrvRe    = '^\s*TDRV(\d+)\s*=\s*(.*)$'
$cpudllRe  = '^\s*(CPUDLL\d+)\s*=\s*(.*)$'
$sections  = @('ARM', 'ARMADS')

$used = @{}
$cpudllRefs = @{}
for ($i = 0; $i -lt $lines.Count; $i++) {
    $m = [regex]::Match($lines[$i], $tdrvRe)
    if ($m.Success) { $used[[int]$m.Groups[1].Value] = $i }
    $m2 = [regex]::Match($lines[$i], $cpudllRe)
    if ($m2.Success) {
        foreach ($t in [regex]::Matches($m2.Groups[2].Value, 'TDRV(\d+)')) { $cpudllRefs[[int]$t.Groups[1].Value] = $true }
    }
}

# existing BlueBridge slot -> reuse it (idempotent install / upgrade)
$slot = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
    $m = [regex]::Match($lines[$i], $tdrvRe)
    if ($m.Success -and $m.Groups[2].Value -match 'BlueBridgeAGDI') { $slot = [int]$m.Groups[1].Value; break }
}
if ($slot -lt 0) {
    for ($n = 0; $n -le 39; $n++) {
        if (-not $used.ContainsKey($n) -and -not $cpudllRefs.ContainsKey($n)) { $slot = $n; break }
    }
}
if ($slot -lt 0) { throw "no free TDRV slot (0..39) in $ini" }
$slotTag = 'TDRV' + $slot
$newLine = '{0}={1} ("{2}")' -f $slotTag, $relPath, $DisplayName
Write-Host ("  TDRV slot : {0}" -f $slotTag)

# where the definition line goes: right after the last NON-BlueBridge TDRV
# line of each toolset section (or right after the section header when the
# section has none). Computed over the original lines so a stale BlueBridge
# line (which is dropped below) can never shift the insertion point.
$insertAfter = @{}
$cur = ''
for ($i = 0; $i -lt $lines.Count; $i++) {
    $ms = [regex]::Match($lines[$i], $sectionRe)
    if ($ms.Success) {
        $cur = $ms.Groups[1].Value
        if (($sections -contains $cur) -and -not $insertAfter.ContainsKey($cur)) { $insertAfter[$cur] = $i }
        continue
    }
    if ($sections -notcontains $cur) { continue }
    $mt = [regex]::Match($lines[$i], $tdrvRe)
    if ($mt.Success -and $mt.Groups[2].Value -notmatch 'BlueBridgeAGDI') { $insertAfter[$cur] = $i }
}

$result  = New-Object System.Collections.Generic.List[string]
$sectionsDone = @{}
$cur = ''
for ($i = 0; $i -lt $lines.Count; $i++) {
    $line = $lines[$i]
    $ms = [regex]::Match($line, $sectionRe)
    if ($ms.Success) { $cur = $ms.Groups[1].Value }

    # stale BlueBridge TDRV lines anywhere are replaced by the canonical line
    # inserted below (also handles a slot change); never duplicated
    $mt = [regex]::Match($line, $tdrvRe)
    if ($mt.Success -and $mt.Groups[2].Value -match 'BlueBridgeAGDI') { continue }

    # keep the slot listed in the Cortex-M CPUDLL group of [ARM]/[ARMADS]
    $mc = [regex]::Match($line, $cpudllRe)
    if ($mc.Success -and ($sections -contains $cur) -and $line -match 'SARMCM3') {
        if ($line -notmatch ('\b' + $slotTag + '\b')) {
            $line = [regex]::Replace($line, '\)(\s*(#.*)?)$', (',' + $slotTag + ')$1'))
        }
    }
    $result.Add($line)

    if (($sections -contains $cur) -and $insertAfter.ContainsKey($cur) -and
        $insertAfter[$cur] -eq $i -and -not $sectionsDone.ContainsKey($cur)) {
        $result.Add($newLine)
        $sectionsDone[$cur] = $true
    }
}

# idempotent by construction: compare the rebuilt file against the original
$unchanged = ($result.Count -eq $lines.Count)
if ($unchanged) {
    for ($k = 0; $k -lt $lines.Count; $k++) {
        if ($result[$k] -ne $lines[$k]) { $unchanged = $false; break }
    }
}
if (-not $unchanged) {
    $stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'
    $backup = "$ini.bluebridge-$stamp.bak"
    if (Test-Path $backup) { $backup = "$ini.bluebridge-$stamp-" + (Get-Random -Minimum 100 -Maximum 999) + ".bak" }
    Copy-Item -LiteralPath $ini -Destination $backup -Force
    try {
        [System.IO.File]::WriteAllLines($ini, $result, (New-Object System.Text.UTF8Encoding($false)))
    } catch {
        throw "Access denied writing '$ini'. Please run PowerShell as Administrator and retry. ($($_.Exception.Message))"
    }
    Write-Host ("  backup    : {0}" -f $backup)
} else {
    Write-Host "  TOOLS.INI : already up to date (no change, no backup)"
}

Write-Host ""
Write-Host "--- TOOLS.INI changes ---" -ForegroundColor Cyan
if ($unchanged) {
    Write-Host "(no change needed)"
} else {
    $oldSet = @{}; foreach ($l in $lines)  { $oldSet[$l] = 1 }
    $newSet = @{}; foreach ($l in $result) { $newSet[$l] = 1 }
    foreach ($l in $result) {
        if (-not $oldSet.ContainsKey($l) -and $l -ne $newLine) { Write-Host ("+ " + $l) }
    }
    foreach ($s in $sections) {
        if ($sectionsDone.ContainsKey($s)) { Write-Host ("+ " + $newLine + "   (section [$s])") }
    }
    foreach ($l in $lines) {
        if (-not $newSet.ContainsKey($l)) { Write-Host ("- " + $l) }
    }
}

# --------------------------------------------------------------------------
# 5. agdi.ini (driver configuration, absolute SimulatorPath)
# --------------------------------------------------------------------------
if (-not $SkipAgdiIni) {
    $agdiIniDir = Join-Path $env:LOCALAPPDATA 'BlueBridgeSimulator'
    $agdiIni = Join-Path $agdiIniDir 'agdi.ini'
    New-Item -ItemType Directory -Path $agdiIniDir -Force | Out-Null

    $lines = @()
    if (Test-Path $agdiIni) { $lines = @(Get-Content -LiteralPath $agdiIni) }

    $wanted = [ordered]@{}
    if ($SimulatorPath) { $wanted['SimulatorPath'] = $SimulatorPath }
    $wanted['AutoStart'] = '1'
    $wanted['LeaveSimulatorRunning'] = '1'
    $wanted['ConnectTimeoutMs'] = '10000'
    $wanted['RequestTimeoutMs'] = '2000'
    $wanted['LoadTrace'] = '0'

    # keep user keys (PipeName, Trace, ...) but drop keys we never want
    $kept = New-Object System.Collections.Generic.List[string]
    $secSeen = $false
    $done = @{}
    foreach ($l in $lines) {
        if ($l -match '^\s*\[(.+?)\]\s*$') {
            if ($Matches[1] -ne 'BlueBridge') { $kept.Add($l); continue }
            $secSeen = $true
            continue
        }
        $mm = [regex]::Match($l, '^\s*([A-Za-z0-9_]+)\s*=')
        if (-not $mm.Success) { if ($l.Trim()) { $kept.Add($l) }; continue }
        $key = $mm.Groups[1].Value
        if ($key -eq 'ProgramFaultAfterBlock') { continue }   # removed from production
        if ($wanted.Contains($key)) { continue }              # rewritten below
        $kept.Add(('{0}={1}' -f $key, $l.Substring($l.IndexOf('=') + 1).Trim()))
    }

    $outLines = New-Object System.Collections.Generic.List[string]
    $outLines.Add('[BlueBridge]')
    foreach ($k in @('SimulatorPath', 'AutoStart', 'LeaveSimulatorRunning', 'ConnectTimeoutMs', 'RequestTimeoutMs', 'LoadTrace')) {
        if ($wanted.Contains($k)) { $outLines.Add(('{0}={1}' -f $k, $wanted[$k])) }
    }
    foreach ($l in $kept) { if ($l.Trim()) { $outLines.Add($l) } }
    try {
        # Unicode (UTF-16LE with BOM): the driver reads the file with the Win32
        # profile API (GetPrivateProfileStringW), which understands ANSI or
        # Unicode files -- but NOT UTF-8: a UTF-8-encoded non-ASCII path (e.g.
        # a Chinese install directory) would be misread byte-for-byte, and a
        # UTF-8 BOM corrupts the [BlueBridge] section name entirely. Verified
        # empirically in B.4.4 (spec section 27).
        [System.IO.File]::WriteAllLines($agdiIni, $outLines, [System.Text.Encoding]::Unicode)
    } catch {
        throw "Access denied writing '$agdiIni'. Please run PowerShell as Administrator and retry. ($($_.Exception.Message))"
    }
    Write-Host ""
    Write-Host "--- agdi.ini ---" -ForegroundColor Cyan
    Write-Host ("{0}" -f $agdiIni)
    if ($SimulatorPath) { Write-Host ("SimulatorPath={0}" -f $SimulatorPath) }
}

# --------------------------------------------------------------------------
# 6. completion summary (spec section 65)
# --------------------------------------------------------------------------
Write-Host ""
Write-Host "BlueBridge Simulator installation complete" -ForegroundColor Green
Write-Host ("  Keil      : {0}" -f $KeilPath)
Write-Host ("  uVision   : {0}" -f $uv4Ver)
Write-Host ("  Driver slot: {0}" -f $slotTag)
Write-Host ("  AGDI      : {0}" -f (Join-Path $targetDir 'BlueBridgeAGDI.dll'))
if ($SimulatorPath) { Write-Host ("  Simulator : {0}" -f $SimulatorPath) }
Write-Host ""
Write-Host "Next (in uVision):" -ForegroundColor Cyan
Write-Host "  Options for Target -> Debug -> Use: '$DisplayName'"
Write-Host "  enable 'Load Application at Startup' and 'Run to main()'"
Write-Host "  Build -> Start Debug Session (Ctrl+F5)"
Write-Host ""
Write-Host ("Undo: powershell -File uninstall.ps1 -KeilPath `"{0}`"" -f $KeilPath) -ForegroundColor DarkGray