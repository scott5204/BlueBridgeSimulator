# BlueBridgeAGDI uninstall script (stage 7-2B.4.4, spec sections 66-73)
#
# Removes ONLY what the installer created:
#   * the TDRV entry whose value contains "BlueBridgeAGDI"  (found by content,
#     never by a hard-coded slot number)
#   * that slot from every CPUDLL group list (other drivers are untouched)
#   * <KeilRoot>\ARM\BlueBridgeAGDI  (our DLL folder)
#   * %LOCALAPPDATA%\BlueBridgeSimulator\agdi.ini  (our driver config;
#     logs are kept)
#
# It never restores a TOOLS.INI backup as a whole (that would wipe drivers and
# packs the user installed after us), never touches other Keil binaries and
# never deletes user projects / firmware / logs.
#
# Idempotent: when nothing is installed it prints "not installed" and exits 0.

[CmdletBinding()]
param(
    [Alias('KeilRoot')][string]$KeilPath = ""
)

$ErrorActionPreference = 'Stop'

function Find-Keil([string]$Hint) {
    $probe = New-Object System.Collections.Generic.List[string]
    if ($Hint) { $probe.Add($Hint) }
    foreach ($key in @(
        'HKCU:\SOFTWARE\Keil\Products\MDK',
        'HKLM:\SOFTWARE\WOW6432Node\Keil\Products\MDK',
        'HKLM:\SOFTWARE\Keil\Products\MDK')) {
        try {
            $p = (Get-ItemProperty -Path $key -ErrorAction Stop).Path
            if ($p) { $probe.Add($p) }
        } catch { }
    }
    $probe.Add((Join-Path $env:LOCALAPPDATA 'Keil_v5'))
    $probe.Add('C:\Keil_v5')
    foreach ($p in $probe) {
        if (-not $p) { continue }
        $p = $p.TrimEnd('\')
        if ((Test-Path (Join-Path $p 'UV4\UV4.exe')) -and (Test-Path (Join-Path $p 'TOOLS.INI'))) { return $p }
    }
    return ''
}

if (-not $KeilPath) { $KeilPath = Find-Keil '' }
if (-not $KeilPath) {
    Write-Host "Keil installation not found; nothing to uninstall. Pass -KeilPath if it is installed somewhere unusual."
    exit 0
}
$KeilPath = (Resolve-Path -LiteralPath $KeilPath).Path
$ini    = Join-Path $KeilPath 'TOOLS.INI'
$armDir = Join-Path $KeilPath 'ARM'
if (-not (Test-Path $ini)) {
    Write-Host "TOOLS.INI not found at $ini; nothing to uninstall."
    exit 0
}

$lines = @(Get-Content -LiteralPath $ini)
$tdrvRe = '^\s*TDRV(\d+)\s*=\s*(.*)$'

$slots = @()
for ($i = 0; $i -lt $lines.Count; $i++) {
    $m = [regex]::Match($lines[$i], $tdrvRe)
    if ($m.Success -and $m.Groups[2].Value -match 'BlueBridgeAGDI') { $slots += [int]$m.Groups[1].Value }
}
$folder = Join-Path $armDir 'BlueBridgeAGDI'
$agdiIni = Join-Path (Join-Path $env:LOCALAPPDATA 'BlueBridgeSimulator') 'agdi.ini'

if ($slots.Count -eq 0 -and -not (Test-Path $folder)) {
    Write-Host "BlueBridge AGDI driver is not installed (no TDRV entry, no driver folder)." -ForegroundColor Yellow
    if (Test-Path $agdiIni) {
        Remove-Item -LiteralPath $agdiIni -Force
        Write-Host ("removed   : {0} (stale config)" -f $agdiIni)
    }
    exit 0
}

$result  = New-Object System.Collections.Generic.List[string]
$changed = New-Object System.Collections.Generic.List[string]
foreach ($line in $lines) {
    $m = [regex]::Match($line, $tdrvRe)
    if ($m.Success -and $m.Groups[2].Value -match 'BlueBridgeAGDI') {
        $changed.Add("- $line")
        continue
    }
    $new = $line
    foreach ($s in $slots) {
        $tag = 'TDRV' + $s
        if ($new -match ('\b' + $tag + '\b')) {
            $new = $new -replace (',' + $tag + '\b'), ''
            $new = $new -replace ('\b' + $tag + ',?'), ''
        }
    }
    $new = $new -replace ',{2,}', ','   # a removed mid-list ref must not leave ",,"
    if ($new -ne $line) { $changed.Add("- $line"); $changed.Add("+ $new") }
    $result.Add($new)
}

if ($changed.Count -gt 0) {
    $stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'
    $backup = "$ini.bluebridge-uninstall-$stamp.bak"
    if (Test-Path $backup) { $backup = "$ini.bluebridge-uninstall-$stamp-" + (Get-Random -Minimum 100 -Maximum 999) + ".bak" }
    Copy-Item -LiteralPath $ini -Destination $backup -Force
    [System.IO.File]::WriteAllLines($ini, $result, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host ("backup    : {0}" -f $backup)
}

if (Test-Path $folder) {
    Remove-Item -LiteralPath $folder -Recurse -Force
    Write-Host ("removed   : {0}" -f $folder)
}
if (Test-Path $agdiIni) {
    Remove-Item -LiteralPath $agdiIni -Force
    Write-Host ("removed   : {0}" -f $agdiIni)
}

Write-Host ""
Write-Host "--- TOOLS.INI changes ---" -ForegroundColor Cyan
if ($changed.Count -eq 0) { Write-Host "(nothing removed)" } else { $changed | ForEach-Object { Write-Host $_ } }
Write-Host ""
Write-Host "BlueBridge AGDI driver uninstalled (other Keil drivers, packs and projects are untouched)." -ForegroundColor Green
Write-Host "Other debug drivers stay available in Options for Target - Debug."