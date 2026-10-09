# BlueBridgeAGDI build script (stage 7-2B)
#
# Builds BlueBridgeAGDI.dll (Win32/x86, MSVC, static CRT) and verifies the
# export list against the official AGDI requirement (spec section 22).
#
# The platform is derived from the bitness of the local UV4.exe, so the DLL
# always matches µVision (spec section 6).
#
# Examples:
#   powershell -File tools\keil_agdi\build_agdi.ps1
#   powershell -File tools\keil_agdi\build_agdi.ps1 -VsPath D:\...\vsbuildtools -Configuration Debug

[CmdletBinding()]
param(
    [string]$AgdiSdkDir = "",
    [string]$VsPath = "",
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [string]$OutDir = "",
    [string]$Uv4Path = "",
    [string]$Version = "1.0.0",
    [string]$ProductName = "BlueBridgeSimulator CT117E-M4 AGDI Driver"
)

$ErrorActionPreference = 'Stop'

$agdiRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$simRoot  = Split-Path -Parent (Split-Path -Parent $agdiRoot)   # BlueBridgeSimulator
$repoRoot = Split-Path -Parent $simRoot                         # repo root

if (-not $AgdiSdkDir) { $AgdiSdkDir = Join-Path $agdiRoot 'vendor\agdi_sdk\SampTargN' }
if (-not $OutDir)     { $OutDir     = Join-Path $agdiRoot ('build\' + $Configuration) }

$srcDir = Join-Path $agdiRoot 'BlueBridgeAGDI\src'
$probeSrcDir = Join-Path $agdiRoot 'BlueBridgeAGDIProbe\src'
$wireDir = Join-Path $simRoot 'src\debug\ipc'    # DebugIpcWire.h (wire constants)
$sources = Get-ChildItem $srcDir -Filter *.cpp | Sort-Object Name | ForEach-Object { $_.FullName }

Write-Host "== BlueBridgeAGDI build ==" -ForegroundColor Cyan
Write-Host "  agdi root : $agdiRoot"
Write-Host "  agdi sdk  : $AgdiSdkDir"
Write-Host "  config    : $Configuration"
Write-Host "  out dir   : $OutDir"

# --------------------------------------------------------------------------
# 1. official AGDI header present?
# --------------------------------------------------------------------------
$agdiHeader = Join-Path $AgdiSdkDir 'AGDI.H'
if (-not (Test-Path $agdiHeader)) {
    throw "official AGDI.H not found at '$agdiHeader'. See tools\keil_agdi\vendor\README.md (download apntex_173.zip from keil.com)."
}

# --------------------------------------------------------------------------
# 2. UV4 bitness -> target platform
# --------------------------------------------------------------------------
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

if (-not $Uv4Path) {
    $candidates = @(
        (Join-Path $env:LOCALAPPDATA 'Keil_v5\UV4\UV4.exe'),
        'C:\Keil_v5\UV4\UV4.exe'
    )
    foreach ($c in $candidates) { if (Test-Path $c) { $Uv4Path = $c; break } }
}

$uv4Machine = 0
if ($Uv4Path) { $uv4Machine = Get-PeMachine $Uv4Path }
switch ($uv4Machine) {
    0x014C  { $platform = 'Win32'; $vsArch = 'x86';  $machine = 'X86';   $hostArch = 'x86' }
    0x8664  { $platform = 'x64';   $vsArch = 'x64';  $machine = 'X64';   $hostArch = 'x64' }
    default {
        Write-Warning "UV4.exe not found (or unknown PE machine) - defaulting to Win32/x86 (this machine's UV4 is 32-bit)."
        $platform = 'Win32'; $vsArch = 'x86'; $machine = 'X86'; $hostArch = 'x86'
    }
}
Write-Host ("  UV4       : {0} (PE machine 0x{1:X4}) -> platform {2}" -f $Uv4Path, $uv4Machine, $platform)

# --------------------------------------------------------------------------
# 3. locate Visual Studio / Build Tools
# --------------------------------------------------------------------------
function Find-Vs([string]$Hint) {
    $probe = @()
    if ($Hint) { $probe += $Hint }
    if ($env:VSINSTALLDIR) { $probe += $env:VSINSTALLDIR.TrimEnd('\') }
    $probe += (Join-Path $repoRoot 'tools\vsbuildtools')
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $found = & $vswhere -products * -prerelease -latest -property installationPath 2>$null
        if ($found) { $probe += ($found | Select-Object -First 1) }
    }
    foreach ($p in $probe) {
        if (-not $p) { continue }
        $vcvars = Join-Path $p 'VC\Auxiliary\Build\vcvarsall.bat'
        if (Test-Path $vcvars) { return @{ Root = $p; VcVars = $vcvars } }
    }
    return $null
}

$vs = Find-Vs $VsPath
if (-not $vs) {
    throw "No Visual Studio / Build Tools with C++ found. Install 'Microsoft.VisualStudio.2022.BuildTools' (VCTools workload) or pass -VsPath."
}
Write-Host "  VS        : $($vs.Root)"

# --------------------------------------------------------------------------
# 4. compile + link (cl /LD, static CRT per the official template: /MT, /MTd)
# --------------------------------------------------------------------------
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

# --------------------------------------------------------------------------
# 4a. Windows version resource (spec B.4.4 section 41: FileVersion /
#     ProductVersion / ProductName metadata; no fake company name)
# --------------------------------------------------------------------------
$verParts = @($Version -split '\.')
while ($verParts.Count -lt 4) { $verParts += '0' }
$vv = ($verParts[0..3] -join ',')
$rcText = @"
#include <winver.h>
VS_VERSION_INFO VERSIONINFO
 FILEVERSION $vv
 PRODUCTVERSION $vv
 FILEFLAGSMASK 0x3fL
 FILEFLAGS 0x0L
 FILEOS 0x40004L
 FILETYPE 0x2L
 FILESUBTYPE 0x0L
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "CompanyName", "BlueBridgeSimulator"
            VALUE "FileDescription", "$ProductName"
            VALUE "FileVersion", "$Version"
            VALUE "InternalName", "BlueBridgeAGDI"
            VALUE "OriginalFilename", "BlueBridgeAGDI.dll"
            VALUE "ProductName", "$ProductName"
            VALUE "ProductVersion", "$Version"
            VALUE "Comments", "Keil uVision AGDI driver for the BlueBridge CT117E-M4 virtual board. Unsigned binary."
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
"@
$rcPath = Join-Path $OutDir 'BlueBridgeAGDI.rc'
[System.IO.File]::WriteAllText($rcPath, $rcText, (New-Object System.Text.ASCIIEncoding))
$rcCmd = 'call "' + $vs.VcVars + '" ' + $vsArch + ' && cd /d "' + $OutDir + '" && rc /nologo /fo BlueBridgeAGDI.res BlueBridgeAGDI.rc'
cmd.exe /c $rcCmd
if ($LASTEXITCODE -ne 0) { throw "rc.exe (version resource) failed with exit code $LASTEXITCODE" }
Write-Host ("  version   : {0} (BlueBridgeAGDI.res)" -f $Version) -ForegroundColor DarkGray

$crt     = if ($Configuration -eq 'Release') { '/MT' }  else { '/MTd' }
$opt     = if ($Configuration -eq 'Release') { '/O2 /DNDEBUG' } else { '/Od /Zi /D_DEBUG' }
$pdbArg  = if ($Configuration -eq 'Debug') { '/Fd:BlueBridgeAGDI.pdb' } else { '/Fd:BlueBridgeAGDI.pdb' }

$clArgs = @(
    '/nologo', '/LD', '/W4', '/EHsc',
    '/D_CRT_SECURE_NO_WARNINGS', '/DWIN32', '/D_WINDOWS', '/D_USRDLL',
    $crt
) + ($opt -split ' ') + @(
    ('/I"' + $AgdiSdkDir + '"'),
    ('/I"' + $srcDir + '"'),
    ('/I"' + $wireDir + '"'),
    $pdbArg
) + ($sources | ForEach-Object { '"' + $_ + '"' }) + @(
    'BlueBridgeAGDI.res',
    '/link', ('/MACHINE:' + $machine), '/SUBSYSTEM:WINDOWS',
    'user32.lib',   # PostMessageW(MSG_UV2_TERMINATE) -- official AGDI.CPP
    '/OPT:REF', '/OPT:ICF', '/INCREMENTAL:NO',
    '/OUT:BlueBridgeAGDI.dll'
)

$cmdLine = 'call "' + $vs.VcVars + '" ' + $vsArch + ' && cd /d "' + $OutDir + '" && cl ' + ($clArgs -join ' ')
Write-Host "  cl        : $($clArgs[0..6] -join ' ') ..." -ForegroundColor DarkGray

cmd.exe /c $cmdLine
$clExit = $LASTEXITCODE
if ($clExit -ne 0) { throw "cl.exe/link.exe failed with exit code $clExit" }

$dll = Join-Path $OutDir 'BlueBridgeAGDI.dll'
if (-not (Test-Path $dll)) { throw "build reported success but $dll is missing" }

$dllMachine = Get-PeMachine $dll
Write-Host ("  built     : $dll (PE machine 0x{0:X4})" -f $dllMachine) -ForegroundColor Green
if ($uv4Machine -ne 0 -and $dllMachine -ne $uv4Machine) {
    throw ("bitness mismatch: DLL 0x{0:X4} vs UV4 0x{1:X4}" -f $dllMachine, $uv4Machine)
}

# --------------------------------------------------------------------------
# 5. verify the export list (spec section 22: dumpbin /exports)
# --------------------------------------------------------------------------
$required = @(
    'EnumUvARM7', 'DllUv3Cap',
    'AG_Init', 'AG_MemAtt', 'AG_BpInfo', 'AG_BreakFunc', 'AG_GoStep',
    'AG_Serial', 'AG_MemAcc', 'AG_RegAcc', 'AG_AllReg', 'AG_HistFunc'
)

$dumpCmd = 'call "' + $vs.VcVars + '" ' + $vsArch + ' && cd /d "' + $OutDir + '" && dumpbin /nologo /exports BlueBridgeAGDI.dll'
$exportsText = cmd.exe /c $dumpCmd 2>&1 | Out-String
Set-Content -Path (Join-Path $OutDir 'exports.txt') -Value $exportsText -Encoding ASCII

$missing = @()
foreach ($name in $required) {
    if ($exportsText -notmatch ('(?m)^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+' + [regex]::Escape($name) + '\s*$')) {
        $missing += $name
    }
}
if ($missing.Count -gt 0) {
    Write-Host $exportsText
    throw ("export check failed, missing: " + ($missing -join ', '))
}

Write-Host "  exports   : $($required.Count)/$($required.Count) official names present (exports.txt)" -ForegroundColor Green

# --------------------------------------------------------------------------
# 5b. dependency check (spec B.4.4 sections 29/111/304): /MT static CRT means
#     the driver needs NO VC runtime, and it must not link Qt/Unicorn/MinGW.
# --------------------------------------------------------------------------
$depsCmd = 'call "' + $vs.VcVars + '" ' + $vsArch + ' && cd /d "' + $OutDir + '" && dumpbin /nologo /dependents BlueBridgeAGDI.dll'
$depsText = cmd.exe /c $depsCmd 2>&1 | Out-String
Set-Content -Path (Join-Path $OutDir 'dependents.txt') -Value $depsText -Encoding ASCII
$forbidden = @('VCRUNTIME', 'MSVCP', 'ucrtbase', 'Qt6', 'unicorn', 'libstdc++', 'libgcc', 'libwinpthread')
$hits = @()
foreach ($p in $forbidden) {
    if ($depsText -match ('(?i)' + [regex]::Escape($p))) { $hits += $p }
}
if ($hits.Count -gt 0) {
    Write-Host $depsText
    throw ("dependency check failed, unexpected imports: " + ($hits -join ', '))
}
Write-Host "  dependents: no VC runtime / Qt / Unicorn / MinGW imports (dependents.txt)" -ForegroundColor Green

# --------------------------------------------------------------------------
# 6. BlueBridgeAGDIProbe.exe (console, Win32/x86, /MT)
#    Reuses the SAME IpcClient/IpcProtocol/wire constants; it does not use the
#    AGDI API and never links simulator code (spec 7-2B.2 sections 5/6/38).
# --------------------------------------------------------------------------
$probeSources = @(
    (Join-Path $srcDir 'IpcProtocol.cpp'),
    (Join-Path $srcDir 'IpcClient.cpp')
) + (Get-ChildItem $probeSrcDir -Filter *.cpp | Sort-Object Name | ForEach-Object { $_.FullName })

foreach ($f in $probeSources) {
    if (-not (Test-Path $f)) { throw "probe source missing: $f" }
}

$probeArgs = @(
    '/nologo', '/W4', '/EHsc', '/D_CRT_SECURE_NO_WARNINGS',
    '/DWIN32', '/D_WINDOWS', '/D_CONSOLE', $crt
) + ($opt -split ' ') + @(
    ('/I"' + $srcDir + '"'),
    ('/I"' + $wireDir + '"'),
    ('/Fe:BlueBridgeAGDIProbe.exe'),
    ('/Fd:BlueBridgeAGDIProbe.pdb')
) + ($probeSources | ForEach-Object { '"' + $_ + '"' }) + @(
    '/link', ('/MACHINE:' + $machine), '/SUBSYSTEM:CONSOLE',
    '/OPT:REF', '/OPT:ICF', '/INCREMENTAL:NO'
)

$probeCmd = 'call "' + $vs.VcVars + '" ' + $vsArch + ' && cd /d "' + $OutDir + '" && cl ' + ($probeArgs -join ' ')
cmd.exe /c $probeCmd
if ($LASTEXITCODE -ne 0) { throw "probe build failed with exit code $LASTEXITCODE" }

$probeExe = Join-Path $OutDir 'BlueBridgeAGDIProbe.exe'
if (-not (Test-Path $probeExe)) { throw "probe build reported success but $probeExe is missing" }
$probeMachine = Get-PeMachine $probeExe
Write-Host ("  probe     : $probeExe (PE machine 0x{0:X4})" -f $probeMachine) -ForegroundColor Green
if ($uv4Machine -ne 0 -and $probeMachine -ne $uv4Machine) {
    throw ("probe bitness mismatch: 0x{0:X4} vs UV4 0x{1:X4}" -f $probeMachine, $uv4Machine)
}

Write-Host ""
Write-Host "Next steps:" -ForegroundColor Cyan
Write-Host "  1. probe (no Keil):  tools\keil_agdi\build\$Configuration\BlueBridgeAGDIProbe.exe"
Write-Host "  2. install driver :  tools\keil_agdi\install.ps1 -DllPath `"$dll`""