# BlueBridge AGDI -- B.4.4.4 installer / uninstaller stress (spec sections 46-73).
#
# Runs install -> idempotent install -> uninstall -> idempotent uninstall for
# N rounds against the REAL per-user Keil installation and proves after every
# step that:
#   * TOOLS.INI changes are exactly the BlueBridge ones (one TDRV definition in
#     [ARM] and one in [ARMADS], one slot reference in each Cortex-M CPUDLL1
#     group) -- every other line is byte-identical to the pre-install file;
#   * the file is byte-stable across rounds (no drift / corruption / lost
#     entries / duplicate TDRV numbers / duplicate CPUDLL references);
#   * install is idempotent (reuses the same slot, "already up to date",
#     no new backup) and repairs an upgrade (restores a corrupted DLL and a
#     wrong SimulatorPath without touching TOOLS.INI);
#   * uninstall removes ONLY the BlueBridge entry, driver folder and config;
#     user canaries (a fake user project in the Keil tree and a user file in
#     %LOCALAPPDATA%\BlueBridgeSimulator) and the driver logs survive;
#   * both scripts are idempotent (second run changes nothing, no new backup).
#
# MUST run outside the agent sandbox (writes the per-user Keil installation).
#
# Usage:
#   powershell -File tools\keil_agdi\run_installer_stress.ps1            # 20 rounds
#   powershell -File tools\keil_agdi\run_installer_stress.ps1 -Rounds 5
[CmdletBinding()]
param(
    [int]$Rounds = 20,
    [string]$KeilPath = ""
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$install   = Join-Path $scriptDir 'install.ps1'
$uninstall = Join-Path $scriptDir 'uninstall.ps1'
$dllSrc    = Join-Path $scriptDir 'build\Release\BlueBridgeAGDI.dll'
$outDir    = Join-Path $scriptDir 'build'
$report    = Join-Path $outDir 'b44_installer_stress.txt'
$localCfg  = Join-Path $env:LOCALAPPDATA 'BlueBridgeSimulator'
$agdiIni   = Join-Path $localCfg 'agdi.ini'
$uv4       = Join-Path $env:LOCALAPPDATA 'Keil_v5\UV4\UV4.exe'

if (-not $KeilPath) {
    if (Test-Path $uv4) { $KeilPath = Split-Path (Split-Path $uv4) -Parent }
    else { throw "uVision not found; pass -KeilPath" }
}
$toolsIni   = Join-Path $KeilPath 'TOOLS.INI'
$instDll    = Join-Path $KeilPath 'ARM\BlueBridgeAGDI\BlueBridgeAGDI.dll'
$instFolder = Join-Path $KeilPath 'ARM\BlueBridgeAGDI'
if (-not (Test-Path $toolsIni)) { throw "TOOLS.INI not found: $toolsIni" }
if (-not (Test-Path $dllSrc))   { throw "build the driver first (build_agdi.ps1): $dllSrc" }
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$script:checks = @()
function Check([string]$name, [bool]$cond, [string]$detail = '') {
    $script:checks += [pscustomobject]@{ Name = $name; Ok = $cond; Detail = $detail }
    $tag = if ($cond) { '[ok]  ' } else { '[FAIL]' }
    $suffix = if ($detail) { "  ($detail)" } else { '' }
    Write-Host ("    {0} {1}{2}" -f $tag, $name, $suffix)
}

function Invoke-Script([string]$path, [string[]]$extraArgs = @()) {
    $callArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $path) + $extraArgs
    $text = (& powershell @callArgs 2>&1 | Out-String)
    return [pscustomobject]@{ Text = $text; Exit = $LASTEXITCODE }
}

function Get-IniInfo([string]$path) {
    $lines = @(Get-Content -LiteralPath $path)
    $cur = ''; $defs = @(); $cpu = @()
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*\[(.+?)\]\s*$') { $cur = $Matches[1] }
        if ($lines[$i] -match '^\s*TDRV(\d+)\s*=\s*(.*)$') {
            $defs += [pscustomobject]@{ Line = $i + 1; Section = $cur; Slot = [int]$Matches[1]; Text = $lines[$i].Trim() }
        }
        if ($lines[$i] -match '^\s*(CPUDLL\d+)\s*=\s*(.*)$') {
            $cpu += [pscustomobject]@{ Line = $i + 1; Section = $cur; Name = $Matches[1]; Text = $lines[$i].Trim() }
        }
    }
    $bb = @($defs | Where-Object { $_.Text -match 'BlueBridgeAGDI' })
    return [pscustomobject]@{ Lines = $lines; Defs = $defs; Bb = $bb; Cpudll = $cpu }
}

function Get-Normalized([string[]]$lines) {
    # comparable view of the file with every BlueBridge line removed; for the
    # states this stress compares (both without BlueBridge) it is the text
    # itself, but the filter also covers "installed" states if ever compared
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($l in $lines) {
        if ($l -match '^\s*TDRV\d+\s*=.*BlueBridgeAGDI') { continue }
        $out.Add($l)
    }
    return ($out -join "`n")
}

function Get-Sha([string]$path) {
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
}

function Get-SlotRefCount($info, [string]$section, [int]$slot) {
    $n = 0
    foreach ($c in $info.Cpudll) {
        if ($c.Section -eq $section -and $c.Name -eq 'CPUDLL1' -and $c.Text -match 'SARMCM3') {
            $n += ([regex]::Matches($c.Text, ('\bTDRV' + $slot + '\b'))).Count
        }
    }
    return $n
}

function Test-NoDuplicateRefs($info) {
    foreach ($c in $info.Cpudll) {
        $tokens = [regex]::Matches($c.Text, 'TDRV\d+') | ForEach-Object { $_.Value }
        if ((@($tokens | Sort-Object -Unique).Count) -ne $tokens.Count) { return $false }
    }
    foreach ($g in ($info.Defs | Group-Object Section)) {
        $slots = @($g.Group | ForEach-Object { $_.Slot })
        if ((@($slots | Sort-Object -Unique).Count) -ne $slots.Count) { return $false }
    }
    return $true
}

function Read-AgdiIni([string]$path) {
    if (-not ('BBIniRead' -as [type])) {
        Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class BBIniRead { [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern int GetPrivateProfileStringW(string s, string k, string d, StringBuilder r, int n, string f); }
"@
    }
    $sb = New-Object System.Text.StringBuilder 2048
    [BBIniRead]::GetPrivateProfileStringW('BlueBridge', 'SimulatorPath', 'MISSING', $sb, 2048, $path) | Out-Null
    return $sb.ToString()
}

$backupFilter = 'TOOLS.INI.bluebridge-*.bak'
function Get-BackupCount { @(Get-ChildItem -LiteralPath $KeilPath -Filter $backupFilter -ErrorAction SilentlyContinue).Count }

# ==========================================================================
# environment guards
# ==========================================================================
$lines = New-Object System.Collections.Generic.List[string]
function Out([string]$t) { $lines.Add($t); Write-Host $t }
$sw = [System.Diagnostics.Stopwatch]::StartNew()
Out ("=== B.4.4.4 installer/uninstaller stress ({0} rounds, spec 46-73) ===" -f $Rounds)
Out ("Keil root : {0}" -f $KeilPath)
Out ("DLL       : {0}" -f $dllSrc)
Out ("started   : {0}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
Out ""

if (@(Get-Process -Name UV4 -ErrorAction SilentlyContinue).Count -gt 0) {
    throw "uVision is running - close it before the installer stress."
}

$dllHashSrc = Get-Sha $dllSrc
$simPath = @(
    (Join-Path (Split-Path (Split-Path $scriptDir) -Parent) 'build\bluesim.exe'),   # simRoot\build
    (Join-Path (Split-Path (Split-Path $scriptDir) -Parent) 'release\bluesim.exe')
) | Where-Object { Test-Path $_ } | Select-Object -First 1
Out ("expected SimulatorPath : {0}" -f $simPath)
Out ""

# user canaries (must survive every uninstall, spec 71)
$canaryKeil = Join-Path $KeilPath 'bluebridge_user_canary.uvprojx'
$canaryCfg  = Join-Path $localCfg 'user_canary.txt'
Set-Content -LiteralPath $canaryKeil -Value 'user project canary - must never be deleted' -Encoding ASCII
Set-Content -LiteralPath $canaryCfg  -Value 'user config canary - must never be deleted' -Encoding ASCII
$logsDir = Join-Path $localCfg 'logs'

# ==========================================================================
# pre-state: uninstall whatever is there, take the baseline
# ==========================================================================
$u0 = Invoke-Script $uninstall
Check 'pre-state uninstall exits 0' ($u0.Exit -eq 0) ("exit=$($u0.Exit)")
$preInfo = Get-IniInfo $toolsIni
Check 'pre-state has no BlueBridge entry' ($preInfo.Bb.Count -eq 0)
$iniBytes0 = [System.IO.File]::ReadAllBytes($toolsIni)
$iniSha0 = Get-Sha $toolsIni
$norm0 = Get-Normalized $preInfo.Lines
$defsNonBb0 = @($preInfo.Defs | Where-Object { $_.Text -notmatch 'BlueBridgeAGDI' } | ForEach-Object { $_.Text } | Sort-Object)
$backupsAtStart = Get-BackupCount
Out ("baseline  : sha256={0}  nonBB-TDRVs={1}  backups={2}" -f $iniSha0.Substring(0, 16), $defsNonBb0.Count, $backupsAtStart)
Out ""

$slotFirst = -1
$roundResults = @()

# ==========================================================================
# rounds
# ==========================================================================
for ($r = 1; $r -le $Rounds; $r++) {
    $script:checks = @()
    Write-Host ("=== round {0}/{1}" -f $r, $Rounds) -ForegroundColor Cyan

    # ---- round start must equal the baseline bytes (no drift across rounds)
    $shaNow = Get-Sha $toolsIni
    Check 'round start: TOOLS.INI identical to baseline' ($shaNow -eq $iniSha0) ("sha={0}" -f $shaNow.Substring(0, 16))
    $startInfo = Get-IniInfo $toolsIni
    Check 'round start: still uninstalled' ($startInfo.Bb.Count -eq 0)

    # ---- install
    $ins = Invoke-Script $install
    Check 'install exits 0' ($ins.Exit -eq 0) ("exit=$($ins.Exit)")
    Check 'install reports completion' ($ins.Text -match 'installation complete')
    Check 'install reports the TDRV slot' ($ins.Text -match 'TDRV slot\s*:\s*TDRV\d+')

    $info = Get-IniInfo $toolsIni
    $armDefs  = @($info.Bb | Where-Object { $_.Section -eq 'ARM' })
    $adsDefs  = @($info.Bb | Where-Object { $_.Section -eq 'ARMADS' })
    Check 'exactly one BlueBridge TDRV in [ARM]'    ($armDefs.Count -eq 1) ("count=$($armDefs.Count)")
    Check 'exactly one BlueBridge TDRV in [ARMADS]' ($adsDefs.Count -eq 1) ("count=$($adsDefs.Count)")
    $slot = -1
    if ($armDefs.Count -eq 1 -and $adsDefs.Count -eq 1) {
        $slot = $armDefs[0].Slot
        Check 'both sections use the same slot' ($adsDefs[0].Slot -eq $slot) ("slot=$slot")
        if ($slotFirst -lt 0) { $slotFirst = $slot }
        Check 'slot is stable across rounds' ($slot -eq $slotFirst) ("slot=$slot first=$slotFirst")
        Check 'slot in 0..39' ($slot -ge 0 -and $slot -le 39)
    }
    if ($slot -ge 0) {
        Check 'CPUDLL1 [ARM] references the slot once' `
              ((Get-SlotRefCount $info 'ARM' $slot) -eq 1) ("refs=$(Get-SlotRefCount $info 'ARM' $slot)")
        Check 'CPUDLL1 [ARMADS] references the slot once' `
              ((Get-SlotRefCount $info 'ARMADS' $slot) -eq 1) ("refs=$(Get-SlotRefCount $info 'ARMADS' $slot)")
    }
    Check 'no duplicate TDRV numbers / CPUDLL refs' (Test-NoDuplicateRefs $info)
    $defsNonBbAfter = @($info.Defs | Where-Object { $_.Text -notmatch 'BlueBridgeAGDI' } | ForEach-Object { $_.Text } | Sort-Object)
    Check 'all other TDRV entries unchanged' `
          ((($defsNonBbAfter -join "`n") -eq ($defsNonBb0 -join "`n"))) `
          ("count {0} -> {1}" -f $defsNonBb0.Count, $defsNonBbAfter.Count)

    Check 'installed DLL hash == build output' ((Get-Sha $instDll) -eq $dllHashSrc)
    if (Test-Path $agdiIni) {
        $iniB = [System.IO.File]::ReadAllBytes($agdiIni)
        Check 'agdi.ini written as Unicode (UTF-16LE BOM)' ($iniB.Length -ge 2 -and $iniB[0] -eq 0xFF -and $iniB[1] -eq 0xFE)
        $iniText = Get-Content -LiteralPath $agdiIni -Raw
        Check 'agdi.ini has no ProgramFaultAfterBlock' (-not ($iniText -match 'ProgramFaultAfterBlock'))
        Check 'agdi.ini AutoStart=1' ($iniText -match '(?m)^\s*AutoStart\s*=\s*1')
        $sp = Read-AgdiIni $agdiIni
        Check 'agdi.ini SimulatorPath correct' ($sp -eq $simPath) ("'$sp'")
    } else {
        Check 'agdi.ini exists after install' $false
    }

    # ---- idempotent install
    $bkBefore = Get-BackupCount
    $shaBefore = Get-Sha $toolsIni
    $ins2 = Invoke-Script $install
    Check '2nd install exits 0' ($ins2.Exit -eq 0)
    Check '2nd install: already up to date' ($ins2.Text -match 'already up to date')
    Check '2nd install: TOOLS.INI unchanged' ((Get-Sha $toolsIni) -eq $shaBefore)
    Check '2nd install: no new backup' ((Get-BackupCount) -eq $bkBefore)

    # ---- upgrade / repair (round 1 only, spec 59)
    if ($r -eq 1) {
        [System.IO.File]::WriteAllBytes($instDll, [byte[]](1..64))
        $t = [System.IO.File]::ReadAllText($agdiIni)
        $t = $t -replace ([regex]::Escape($simPath)), 'C:\Wrong\bluesim.exe'
        [System.IO.File]::WriteAllText($agdiIni, $t, [System.Text.Encoding]::Unicode)
        $ins3 = Invoke-Script $install
        Check 'repair install exits 0' ($ins3.Exit -eq 0)
        Check 'repair install restored the DLL' ((Get-Sha $instDll) -eq $dllHashSrc)
        Check 'repair install fixed SimulatorPath' ((Read-AgdiIni $agdiIni) -eq $simPath)
        Check 'repair install reused the slot (TOOLS.INI unchanged)' ((Get-Sha $toolsIni) -eq $shaBefore)
    }

    # ---- uninstall
    $un = Invoke-Script $uninstall
    Check 'uninstall exits 0' ($un.Exit -eq 0) ("exit=$($un.Exit)")
    Check 'uninstall reports removal' ($un.Text -match 'uninstalled')
    $infoU = Get-IniInfo $toolsIni
    Check 'uninstall removed all BlueBridge TDRV lines' ($infoU.Bb.Count -eq 0)
    if ($slot -ge 0) {
        Check 'uninstall removed the CPUDLL1 [ARM] ref' ((Get-SlotRefCount $infoU 'ARM' $slot) -eq 0)
        Check 'uninstall removed the CPUDLL1 [ARMADS] ref' ((Get-SlotRefCount $infoU 'ARMADS' $slot) -eq 0)
    }
    Check 'uninstall: normalized TOOLS.INI == baseline' ((Get-Normalized $infoU.Lines) -eq $norm0)
    $bytesU = [System.IO.File]::ReadAllBytes($toolsIni)
    Check 'uninstall: TOOLS.INI byte-identical to baseline' `
          (([System.BitConverter]::ToString($bytesU)) -eq ([System.BitConverter]::ToString($iniBytes0)))
    Check 'uninstall removed the driver folder' (-not (Test-Path $instFolder))
    Check 'uninstall removed agdi.ini' (-not (Test-Path $agdiIni))
    Check 'user canary in the Keil tree survived' (Test-Path $canaryKeil)
    Check 'user canary in BlueBridge config survived' (Test-Path $canaryCfg)
    Check 'driver logs survived' ((Test-Path $logsDir))

    # ---- idempotent uninstall
    $bkBeforeU = Get-BackupCount
    $un2 = Invoke-Script $uninstall
    Check '2nd uninstall exits 0' ($un2.Exit -eq 0)
    Check '2nd uninstall: not installed' ($un2.Text -match 'is not installed')
    Check '2nd uninstall: TOOLS.INI unchanged' ((Get-Sha $toolsIni) -eq $iniSha0)
    Check '2nd uninstall: no new backup' ((Get-BackupCount) -eq $bkBeforeU)

    $bad = @($script:checks | Where-Object { -not $_.Ok }).Count
    $roundResults += [pscustomobject]@{ Round = $r; Checks = $script:checks.Count; Failed = $bad }
    Write-Host ("    -> {0}/{1} checks passed" -f ($script:checks.Count - $bad), $script:checks.Count) `
               -ForegroundColor $(if ($bad -eq 0) { 'Green' } else { 'Red' })
    foreach ($c in $script:checks) {
        $st = if ($c.Ok) { 'ok  ' } else { 'FAIL' }
        Out ("  R{0:d2} {1} {2} {3}" -f $r, $st, $c.Name, $c.Detail)
    }
    if ($bad -gt 0) {
        Out ("round {0} FAILED - stopping" -f $r)
        break
    }
}

# ==========================================================================
# leave the machine installed (the product keeps being used here)
# ==========================================================================
Out ""
$script:checks = @()
$finalInstall = Invoke-Script $install
Check 'final install exits 0' ($finalInstall.Exit -eq 0)
$finalInfo = Get-IniInfo $toolsIni
Check 'final state: exactly one BlueBridge line per section' `
      ((@($finalInfo.Bb | Where-Object { $_.Section -eq 'ARM' }).Count -eq 1) -and
       (@($finalInfo.Bb | Where-Object { $_.Section -eq 'ARMADS' }).Count -eq 1))
Check 'final state: DLL installed' ((Test-Path $instDll) -and ((Get-Sha $instDll) -eq $dllHashSrc))
$finalBad = @($script:checks | Where-Object { -not $_.Ok }).Count
foreach ($c in $script:checks) {
    $st = if ($c.Ok) { 'ok  ' } else { 'FAIL' }
    Out ("  FIN {0} {1} {2}" -f $st, $c.Name, $c.Detail)
}

# canary cleanup (they existed through every uninstall; evidence is above)
Remove-Item -LiteralPath $canaryKeil -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $canaryCfg -Force -ErrorAction SilentlyContinue

$sw.Stop()
$totalFailed = ($roundResults | Measure-Object -Property Failed -Sum).Sum
$totalChecks = ($roundResults | Measure-Object -Property Checks -Sum).Sum
Out ""
Out ("=== stress finished: {0} rounds, {1}/{2} checks passed, {3} failed (+{4} final), {5:N1}s ===" -f `
     @($roundResults).Count, ($totalChecks - $totalFailed), $totalChecks, $totalFailed, $finalBad, $sw.Elapsed.TotalSeconds)
Out ("backups now: {0} (created only on real changes)" -f (Get-BackupCount))
Out ("TOOLS.INI sha256: {0}" -f (Get-Sha $toolsIni))
$lines | Set-Content -LiteralPath $report -Encoding UTF8
Write-Host ("report: {0}" -f $report)

if ($totalFailed -gt 0 -or $finalBad -gt 0 -or @($roundResults).Count -ne $Rounds) { exit 1 } else { exit 0 }