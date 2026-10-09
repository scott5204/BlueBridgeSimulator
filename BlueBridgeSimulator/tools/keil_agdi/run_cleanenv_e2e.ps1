# BlueBridgeSimulator -- B.4.4.5 clean-environment / end-to-end QA
# (stage 7-2B.4.4, spec sections 74-98).
#
# Sections:
#   1. standalone RELEASE simulator: System32-only PATH, arbitrary CWD, GUI
#      screenshot, probe over the debug pipe (spec 32/33/74/75)
#   2. fresh install FROM THE RELEASE PACKAGE (dev install removed first) +
#      two auto-start end-to-end sessions (LoadApp + run-to-main, spec 76-79,
#      91, 92, 94) -- the driver must launch the PACKAGED simulator itself
#   3. install from a path WITH SPACES + end-to-end session (spec 95)
#   4. install from a UNICODE (non-ASCII) path + end-to-end session (spec 96)
#   5. two uVision instances, each with its own simulator/pipe (spec 97)
#   6. single-pipe second client = Busy (spec 98)
#   finally: dev-tree install restored (the machine keeps being developed on)
#
# MUST run outside the agent sandbox (writes the per-user Keil installation
# and C:\Temp).
[CmdletBinding()]
param(
    [string]$Pkg = "",
    [string]$TempRoot = 'C:\Temp',
    [int]$TimeoutSec = 60
)
$ErrorActionPreference = 'Stop'

$scriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path          # ...\tools\keil_agdi
$simRoot    = Split-Path -Parent (Split-Path -Parent $scriptDir)      # ...\BlueBridgeSimulator
$outDir     = Join-Path $scriptDir 'build'
$report     = Join-Path $outDir 'b44_cleanenv.txt'
$installDev = Join-Path $scriptDir 'install.ps1'
$uninstall  = Join-Path $scriptDir 'uninstall.ps1'
$probe      = Join-Path $scriptDir 'build\Release\BlueBridgeAGDIProbe.exe'
$uv4        = Join-Path $env:LOCALAPPDATA 'Keil_v5\UV4\UV4.exe'
$agdiIni    = Join-Path $env:LOCALAPPDATA 'BlueBridgeSimulator\agdi.ini'
$agdiLog    = Join-Path $env:LOCALAPPDATA 'BlueBridgeSimulator\logs\BlueBridgeAGDI.log'
if (-not $Pkg) { $Pkg = Join-Path $simRoot 'dist\BlueBridgeSimulator-v1.0.0' }
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$bbTest = Join-Path $scriptDir 'keil_test\bb_test.uvprojx'
$bbFull = Join-Path $scriptDir 'keil_test\bb_full\bb_full.uvprojx'
$bbFullUvoptx = Join-Path $scriptDir 'keil_test\bb_full\bb_full.uvoptx'
$bbFullInit = Join-Path $scriptDir 'keil_test\bb_full\cleanenv_exit.ini'

foreach ($f in @($uv4, $probe, $bbTest, $bbFull)) {
    if (-not (Test-Path $f)) { throw "missing: $f" }
}
if (-not (Test-Path (Join-Path $Pkg 'install.ps1'))) { throw "release package not found: $Pkg" }

$script:sections = New-Object System.Collections.Generic.List[string]
function Sec([string]$t) { $script:sections.Add(("## " + $t)); Write-Host ("== " + $t) -ForegroundColor Cyan }
$script:checks = @()
function Check([string]$name, [bool]$cond, [string]$detail = '') {
    $script:checks += [pscustomobject]@{ Name = $name; Ok = $cond; Detail = $detail }
    $tag = if ($cond) { '[ok]  ' } else { '[FAIL]' }
    $suffix = if ($detail) { "  ($detail)" } else { '' }
    Write-Host ("    {0} {1}{2}" -f $tag, $name, $suffix)
}

function Run-Ps([string]$path) {
    $text = (& powershell -NoProfile -ExecutionPolicy Bypass -File $path 2>&1 | Out-String)
    return [pscustomobject]@{ Text = $text; Exit = $LASTEXITCODE }
}
function Read-LogDelta([string]$Path, [long]$FromOffset) {
    if (-not (Test-Path $Path)) { return '' }
    $size = (Get-Item $Path).Length
    if ($size -lt $FromOffset) { $FromOffset = 0 }
    $fs = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    try { $fs.Seek($FromOffset, 'Begin') | Out-Null
          $sr = New-Object System.IO.StreamReader($fs); $t = $sr.ReadToEnd(); $sr.Close(); return $t
    } finally { $fs.Close() }
}
function Get-LogLen { if (Test-Path $agdiLog) { (Get-Item $agdiLog).Length } else { 0 } }
function Count-Marker([string]$text, [string]$pattern) {
    if (-not $text) { return 0 }
    return ([regex]::Matches($text, [regex]::Escape($pattern))).Count
}
function Stop-Sims([string]$pathLike) {
    Get-CimInstance Win32_Process -Filter "Name='bluesim.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -and $_.CommandLine -like ("*" + $pathLike + "*") } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}
function Stop-AllTestUv4 {
    # only instances running OUR test projects -- never the user's other windows
    Get-CimInstance Win32_Process -Filter "Name='UV4.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -and $_.CommandLine -like '*keil_test*' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

# Screenshot one process' main window (PrintWindow; works for hidden windows).
function Save-WindowShot($process, [string]$path) {
    try {
        $process.Refresh()
        if ($process.HasExited -or $process.MainWindowHandle -eq [IntPtr]::Zero) { return $false }
        Add-Type -AssemblyName System.Drawing
        if (-not ('CleanEnvShot' -as [type])) {
            Add-Type @"
using System; using System.Runtime.InteropServices;
public static class CleanEnvShot {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
        }
        $r = New-Object CleanEnvShot+RECT
        [CleanEnvShot]::GetWindowRect($process.MainWindowHandle, [ref]$r) | Out-Null
        if (($r.R - $r.L) -le 0 -or ($r.B - $r.T) -le 0) { return $false }
        $bmp = New-Object System.Drawing.Bitmap(($r.R - $r.L), ($r.B - $r.T))
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $hdc = $g.GetHdc()
        $ok = [CleanEnvShot]::PrintWindow($process.MainWindowHandle, $hdc, 2)
        if (-not $ok) { $ok = [CleanEnvShot]::PrintWindow($process.MainWindowHandle, $hdc, 0) }
        $g.ReleaseHdc($hdc); $g.Dispose()
        $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
        return $ok
    } catch { return $false }
}

# Re-apply the E2E project options right before EVERY session. uVision may
# rewrite .uvoptx while a session runs, so the previous state is never trusted
# (this mirrors the proven per-session patch of the B.4 download suite).
function Set-UvoptxE2E {
    $t = [System.IO.File]::ReadAllText($bbFullUvoptx)
    $t = [regex]::Replace($t, '<tIfile>[^<]*</tIfile>', '<tIfile>.\cleanenv_exit.ini</tIfile>')
    $t = [regex]::Replace($t, '<tLdApp>[01]</tLdApp>', '<tLdApp>1</tLdApp>')
    $t = [regex]::Replace($t, '<tGomain>[01]</tGomain>', '<tGomain>1</tGomain>')
    [System.IO.File]::WriteAllText($bbFullUvoptx, $t, (New-Object System.Text.UTF8Encoding($false)))
}

# Auto-start end-to-end session: agdi.ini must have AutoStart=1 and no PipeName.
# Before every launch the project options and the "EXIT" command file are
# rewritten from scratch, so a session can never inherit stale state.
# Returns {Exit, Sec, Delta} (Delta = driver-log delta).
function Invoke-AutoStartSession([string]$tag, [string]$proj) {
    Set-UvoptxE2E
    [System.IO.File]::WriteAllText($bbFullInit, "EXIT`r`n", (New-Object System.Text.ASCIIEncoding))
    $logLen = Get-LogLen
    $out = Join-Path $outDir ("cleanenv_{0}_uv4.txt" -f $tag)
    if (Test-Path $out) { Remove-Item $out -Force }
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $pr = Start-Process -FilePath $uv4 -ArgumentList @('-d', $proj, '-j0', '-sg', ('-o' + $out)) `
                        -PassThru -WindowStyle Hidden
    $finished = $pr.WaitForExit($TimeoutSec * 1000)
    if (-not $finished) {
        # keep visual evidence of whatever modal state blocked uVision
        Save-WindowShot $pr (Join-Path $outDir ("cleanenv_{0}_timeout.png" -f $tag)) | Out-Null
        $pr.Kill(); Start-Sleep -Milliseconds 600
    }
    $sw.Stop()
    $delta = Read-LogDelta $agdiLog $logLen
    $initLen = 0
    if (Test-Path $bbFullInit) { $initLen = (Get-Item $bbFullInit).Length }
    $diag = "--- post-session: initExists={0} initBytes={1} uvoptx {2}" -f `
            (Test-Path $bbFullInit), $initLen,
            ([regex]::Match([System.IO.File]::ReadAllText($bbFullUvoptx), '<tIfile>[^<]*</tIfile>').Value)
    Set-Content -LiteralPath (Join-Path $outDir ("cleanenv_{0}_agdi.log" -f $tag)) `
                -Value ($delta + "`r`n" + $diag) -Encoding UTF8
    return [pscustomobject]@{
        Exit = if ($finished) { $pr.ExitCode } else { 'TIMEOUT' }
        Sec = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
        Delta = $delta
    }
}

$bbFullUvoptxBak = "$bbFullUvoptx.cleanenv.bak"
Copy-Item -LiteralPath $bbFullUvoptx -Destination $bbFullUvoptxBak -Force
$envPathBak = $env:PATH
$sw = [System.Diagnostics.Stopwatch]::StartNew()

try {
    # ======================================================================
    Sec "1. standalone release simulator (clean PATH, arbitrary CWD) [32/33/74/75]"
    # ======================================================================
    $relSim = Join-Path $Pkg 'Simulator\bluesim.exe'
    Check 'release package ships Simulator\bluesim.exe' (Test-Path $relSim)
    Check 'release package ships unicorn.dll next to it' (Test-Path (Join-Path $Pkg 'Simulator\unicorn.dll'))

    # headless-ish probe run under System32-only PATH from an arbitrary CWD
    $cleanPipe = 'BlueBridgeSimulator.Debug.CleanEnv'
    Stop-Sims $cleanPipe
    $env:PATH = 'C:\Windows\System32'
    $simProc = Start-Process -FilePath $relSim -ArgumentList @('--debug-pipe', $cleanPipe, '--wait-debugger') `
                             -PassThru -WorkingDirectory $env:TEMP
    $env:PATH = $envPathBak
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline -and
           -not @(Get-ChildItem '\\.\pipe\' -ErrorAction SilentlyContinue | Where-Object { $_.Name -eq $cleanPipe }).Count) {
        Start-Sleep -Milliseconds 100
    }
    $pipeUp = @(Get-ChildItem '\\.\pipe\' -ErrorAction SilentlyContinue | Where-Object { $_.Name -eq $cleanPipe }).Count -eq 1
    Check 'release simulator starts under PATH=System32 from %TEMP%' ($pipeUp -and -not $simProc.HasExited) `
          ("pipe={0} exited={1}" -f $pipeUp, $simProc.HasExited)
    if ($pipeUp) {
        $caps = & $probe --pipe $cleanPipe --read 0x08000000 16 2>&1 | Out-String
        Check 'probe (no Keil, no dev PATH) can read the release target' ($caps -match 'read:.*data=[0-9a-f]{32}') `
              (($caps -split "`r?`n" | Where-Object { $_ -like 'read:*' } | Select-Object -First 1))
        $caps2 = & $probe --pipe $cleanPipe --read32 0xE000ED00 2>&1 | Out-String
        Check 'probe read32 SCB->CPUID works' ($caps2 -match 'read32:.*value=0x[0-9A-Fa-f]+') `
              (($caps2 -split "`r?`n" | Where-Object { $_ -like 'read32:*' } | Select-Object -First 1))
    }
    Stop-Sims $cleanPipe
    Start-Sleep -Milliseconds 300

    # GUI launch (visible screenshot evidence of the standalone board window)
    $env:PATH = 'C:\Windows\System32'
    $guiProc = Start-Process -FilePath $relSim -PassThru -WorkingDirectory $env:TEMP
    $env:PATH = $envPathBak
    $deadline = (Get-Date).AddSeconds(25)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500; $guiProc.Refresh()
        if ($guiProc.HasExited -or $guiProc.MainWindowHandle -ne [IntPtr]::Zero) { break }
    }
    Check 'release simulator GUI opens under clean PATH' ((-not $guiProc.HasExited) -and ($guiProc.MainWindowHandle -ne [IntPtr]::Zero)) `
          ("title='{0}'" -f $guiProc.MainWindowTitle)
    if (-not $guiProc.HasExited) {
        $shot = Join-Path $outDir 'cleanenv_release_gui.png'
        $ok = Save-WindowShot $guiProc $shot
        Check 'release GUI screenshot captured' ($ok -and (Test-Path $shot)) ($shot)
        if ('CleanEnvShot' -as [type]) {
            [CleanEnvShot]::PostMessageW($guiProc.MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        }
        Start-Sleep -Seconds 3
        if (-not $guiProc.HasExited) { $guiProc.Kill() }
    }

    # ======================================================================
    Sec "2. fresh install from the release package + auto-start E2E [76-79/91/92/94]"
    # ======================================================================
    Stop-AllTestUv4
    $u = Run-Ps $uninstall
    Check 'dev install removed first' ($u.Exit -eq 0) ("exit=$($u.Exit)")
    $fresh = Run-Ps (Join-Path $Pkg 'install.ps1')
    Check 'package install.ps1 exits 0' ($fresh.Exit -eq 0) ("exit=$($fresh.Exit)")
    Check 'package install reports completion' ($fresh.Text -match 'installation complete')
    $pkgSim = Join-Path $Pkg 'Simulator\bluesim.exe'
    $iniText = Get-Content -LiteralPath $agdiIni -Raw
    Check 'agdi.ini points at the PACKAGED simulator' ($iniText -match [regex]::Escape($pkgSim)) `
          (($iniText -split "`r?`n" | Where-Object { $_ -match 'SimulatorPath' } | Select-Object -First 1))
    Check 'agdi.ini has AutoStart=1' ($iniText -match '(?m)^\s*AutoStart\s*=\s*1')
    Check 'agdi.ini has no PipeName pin' (-not ($iniText -match '(?m)^\s*PipeName\s*='))
    $instDll = Join-Path (Split-Path (Split-Path $uv4) -Parent) 'ARM\BlueBridgeAGDI\BlueBridgeAGDI.dll'
    $pkgDll = Join-Path $Pkg 'Keil\BlueBridgeAGDI.dll'
    Check 'installed driver == packaged driver' `
          ((Test-Path $instDll) -and ((Get-FileHash $instDll).Hash -eq (Get-FileHash $pkgDll).Hash))

    # E2E session 1+2: bb_full, LoadApp + run-to-main, auto-started packaged sim
    Set-UvoptxE2E

    Stop-Sims 'BlueBridgeSimulator-v1.0.0'
    $ses1 = Invoke-AutoStartSession 'pkg_e2e_1' $bbFull
    Check 'packaged E2E session 1 exit=0' ($ses1.Exit -eq 0) ("exit=$($ses1.Exit), $($ses1.Sec)s")
    Check 'packaged sim auto-started by the driver' `
          ((Count-Marker $ses1.Delta 'autostart launched simulator') -eq 1)
    Check 'auto-started exe IS the packaged simulator' `
          ($ses1.Delta -match [regex]::Escape('BlueBridgeSimulator-v1.0.0\Simulator\bluesim.exe'))
    Check 'session connected (HELLO)' ((Count-Marker $ses1.Delta 'HELLO session=') -ge 1)
    Check 'current AXF really downloaded (PROGRAM_END commit)' `
          ((Count-Marker $ses1.Delta '[PROGRAM] end success') -eq 1)
    Check 'run to main reached (AG_GOTILADR)' ($ses1.Delta -match 'GOTILADR')
    Check 'clean AG_UNINIT at session end' ((Count-Marker $ses1.Delta 'AG_UNINIT') -ge 1)
    Start-Sleep -Seconds 1
    $keptSim = @(Get-CimInstance Win32_Process -Filter "Name='bluesim.exe'" -ErrorAction SilentlyContinue |
                 Where-Object { $_.CommandLine -and $_.CommandLine -like '*BlueBridgeSimulator-v1.0.0*' }).Count
    Check 'LeaveSimulatorRunning=1 kept the packaged simulator' ($keptSim -ge 1) ("kept=$keptSim")

    $ses2 = Invoke-AutoStartSession 'pkg_e2e_2' $bbFull
    Check 'packaged E2E session 2 exit=0 (re-debug works)' ($ses2.Exit -eq 0) ("exit=$($ses2.Exit), $($ses2.Sec)s")
    Check 'session 2 used its own (new) pipe' `
          ((Count-Marker $ses2.Delta 'autostart launched simulator') -eq 1)
    Stop-Sims 'BlueBridgeSimulator-v1.0.0'
    Start-Sleep -Milliseconds 400

    # ======================================================================
    Sec "3. install path with spaces [95]"
    # ======================================================================
    try {
        New-Item -ItemType Directory -Path $TempRoot -Force | Out-Null
    } catch {
        $TempRoot = $env:TEMP
        Write-Host ("  C:\Temp not writable, falling back to {0}" -f $TempRoot) -ForegroundColor Yellow
    }
    $spaceRoot = Join-Path $TempRoot 'BlueBridge Simulator Test'
    if (Test-Path $spaceRoot) { Remove-Item $spaceRoot -Recurse -Force }
    New-Item -ItemType Directory -Path $spaceRoot -Force | Out-Null
    Copy-Item -Path (Join-Path $Pkg '*') -Destination $spaceRoot -Recurse -Force
    $sp = Run-Ps $uninstall
    Check 'uninstall before spaced install exits 0' ($sp.Exit -eq 0)
    $spIns = Run-Ps (Join-Path $spaceRoot 'install.ps1')
    Check 'spaced-path install exits 0' ($spIns.Exit -eq 0) ("exit=$($spIns.Exit)")
    $spaceSim = Join-Path $spaceRoot 'Simulator\bluesim.exe'
    $iniText = Get-Content -LiteralPath $agdiIni -Raw
    Check 'agdi.ini SimulatorPath contains the space path' ($iniText -match [regex]::Escape($spaceSim)) `
          (($iniText -split "`r?`n" | Where-Object { $_ -match 'SimulatorPath' } | Select-Object -First 1))
    Stop-Sims 'BlueBridge Simulator Test'
    $sesS = Invoke-AutoStartSession 'spaces' $bbFull
    Check 'spaced-path E2E session exit=0' ($sesS.Exit -eq 0) ("exit=$($sesS.Exit)")
    Check 'driver started the simulator from the spaced path' `
          ($sesS.Delta -match [regex]::Escape('BlueBridge Simulator Test\Simulator\bluesim.exe'))
    Check 'spaced-path session committed the image' ((Count-Marker $sesS.Delta '[PROGRAM] end success') -eq 1)
    Stop-Sims 'BlueBridge Simulator Test'
    Start-Sleep -Milliseconds 400

    # ======================================================================
    # non-ASCII (Chinese) folder name, built from code points: PowerShell 5.1
    # reads .ps1 files as ANSI, so a UTF-8 literal image would be mis-decoded
    # before the script runs (U+84DD U+6865 U+676F U+6A21 U+62DF U+5668)
    $uniName = -join [char[]]@(0x84DD, 0x6865, 0x676F, 0x6A21, 0x62DF, 0x5668)
    Sec ("4. install path with non-ASCII ({0}) [96]" -f $uniName)
    # ======================================================================
    $uniRoot = Join-Path $TempRoot $uniName
    if (Test-Path $uniRoot) { Remove-Item $uniRoot -Recurse -Force }
    New-Item -ItemType Directory -Path $uniRoot -Force | Out-Null
    Copy-Item -Path (Join-Path $Pkg '*') -Destination $uniRoot -Recurse -Force
    $un = Run-Ps $uninstall
    Check 'uninstall before unicode install exits 0' ($un.Exit -eq 0)
    $uniIns = Run-Ps (Join-Path $uniRoot 'install.ps1')
    Check 'unicode-path install exits 0' ($uniIns.Exit -eq 0) ("exit=$($uniIns.Exit)")
    $uniSim = Join-Path $uniRoot 'Simulator\bluesim.exe'
    # read it back the way the driver does (Win32 profile API)
    if (-not ('CleanEnvIni' -as [type])) {
        Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class CleanEnvIni { [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern int GetPrivateProfileStringW(string s, string k, string d, StringBuilder r, int n, string f); }
"@
    }
    $sb = New-Object System.Text.StringBuilder 2048
    [CleanEnvIni]::GetPrivateProfileStringW('BlueBridge', 'SimulatorPath', 'MISSING', $sb, 2048, $agdiIni) | Out-Null
    Check 'driver reads the unicode SimulatorPath correctly' ($sb.ToString() -eq $uniSim) ("'$($sb.ToString())'")
    Stop-Sims $uniName
    $sesU = Invoke-AutoStartSession 'unicode' $bbFull
    Check 'unicode-path E2E session exit=0' ($sesU.Exit -eq 0) ("exit=$($sesU.Exit)")
    # the launched process is the hard evidence (WMI command lines are real
    # Unicode); the driver log now also carries the path as UTF-8
    $uniSimProc = @(Get-CimInstance Win32_Process -Filter "Name='bluesim.exe'" -ErrorAction SilentlyContinue |
                    Where-Object { $_.CommandLine -and $_.CommandLine -like ('*' + $uniName + '*') }).Count
    Check 'driver started the simulator from the unicode path' ($uniSimProc -ge 1) ("procs=$uniSimProc")
    Check 'unicode-path session committed the image' ((Count-Marker $sesU.Delta '[PROGRAM] end success') -eq 1)
    Stop-Sims $uniName
    Start-Sleep -Milliseconds 400

    # ======================================================================
    Sec "5. two uVision instances, own simulator each [97]"
    # ======================================================================
    $logLen = Get-LogLen
    # both projects are pinned to an "EXIT" command file; write them ourselves
    Set-UvoptxE2E
    [System.IO.File]::WriteAllText($bbFullInit, "EXIT`r`n", (New-Object System.Text.ASCIIEncoding))
    [System.IO.File]::WriteAllText((Join-Path (Split-Path -Parent $bbTest) 'exit_init.ini'), "EXIT`r`n", `
                                   (New-Object System.Text.ASCIIEncoding))
    $outA = Join-Path $outDir 'cleanenv_two_a_uv4.txt'
    $outB = Join-Path $outDir 'cleanenv_two_b_uv4.txt'
    $pa = Start-Process -FilePath $uv4 -ArgumentList @('-d', $bbTest, '-j0', '-sg', ('-o' + $outA)) `
                        -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 700
    $pb = Start-Process -FilePath $uv4 -ArgumentList @('-d', $bbFull, '-j0', '-sg', ('-o' + $outB)) `
                        -PassThru -WindowStyle Hidden
    $fa = $pa.WaitForExit($TimeoutSec * 1000); if (-not $fa) { $pa.Kill() }
    $fb = $pb.WaitForExit($TimeoutSec * 1000); if (-not $fb) { $pb.Kill() }
    $deltaTwo = Read-LogDelta $agdiLog $logLen
    Set-Content -LiteralPath (Join-Path $outDir 'cleanenv_two_agdi.log') -Value $deltaTwo -Encoding UTF8
    Check 'instance A exit=0' ($fa -and $pa.ExitCode -eq 0) ("exit=$(if ($fa) { $pa.ExitCode } else { 'TIMEOUT' })")
    Check 'instance B exit=0' ($fb -and $pb.ExitCode -eq 0) ("exit=$(if ($fb) { $pb.ExitCode } else { 'TIMEOUT' })")
    $autoPipes = @([regex]::Matches($deltaTwo, 'autostart launched simulator') ).Count
    Check 'both instances auto-started a simulator' ($autoPipes -eq 2) ("autostarts=$autoPipes")
    $pipeNames = [regex]::Matches($deltaTwo, 'BlueBridgeSimulator\.Debug\.\d+\.[0-9A-F]+') |
                 ForEach-Object { $_.Value } | Sort-Object -Unique
    $helloCount = ([regex]::Matches($deltaTwo, 'HELLO session=')).Count
    Check 'distinct pipes per instance' (@($pipeNames).Count -ge 2) ("pipes=$(@($pipeNames).Count)")
    Check 'two HELLO sessions' ($helloCount -ge 2) ("hello=$helloCount")
    Stop-Sims $uniName
    Start-Sleep -Milliseconds 400

    # ======================================================================
    Sec "6. single pipe: second client = Busy [98]"
    # ======================================================================
    $busyPipe = 'BlueBridgeSimulator.Debug.BusyCheck'
    $busySim = Start-Process -FilePath $uniSim -ArgumentList @('--debug-pipe', $busyPipe, '--wait-debugger') -PassThru
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline -and
           -not @(Get-ChildItem '\\.\pipe\' -ErrorAction SilentlyContinue | Where-Object { $_.Name -eq $busyPipe }).Count) {
        Start-Sleep -Milliseconds 100
    }
    $c1 = New-Object System.IO.Pipes.NamedPipeClientStream('.', $busyPipe, [System.IO.Pipes.PipeDirection]::InOut)
    $c1.Connect(3000)
    Check 'first client connects' ($c1.IsConnected)
    $busy = $false; $err = ''
    try {
        $c2 = New-Object System.IO.Pipes.NamedPipeClientStream('.', $busyPipe, [System.IO.Pipes.PipeDirection]::InOut)
        $c2.Connect(1500)
        $c2.Dispose()
    } catch { $busy = $true; $err = $_.Exception.Message }
    Check 'second client on the same pipe is refused (Busy)' $busy $err
    $c1.Dispose()
    Stop-Process -Id $busySim.Id -Force -ErrorAction SilentlyContinue

    # ======================================================================
    Sec "final: dev-tree install restored"
    # ======================================================================
    $fu = Run-Ps $uninstall
    Check 'final uninstall exits 0' ($fu.Exit -eq 0)
    $fi = Run-Ps $installDev
    Check 'final dev install exits 0' ($fi.Exit -eq 0)
    $iniText = Get-Content -LiteralPath $agdiIni -Raw
    $devSim = Join-Path $simRoot 'build\bluesim.exe'
    Check 'dev SimulatorPath restored' ($iniText -match [regex]::Escape($devSim))
    $sesF = Invoke-AutoStartSession 'final_dev' $bbFull
    Check 'final dev E2E session exit=0' ($sesF.Exit -eq 0) ("exit=$($sesF.Exit)")
    Stop-Sims 'BlueBridgeSimulator\build'
    Start-Sleep -Milliseconds 300
} finally {
    $env:PATH = $envPathBak
    Copy-Item -LiteralPath $bbFullUvoptxBak -Destination $bbFullUvoptx -Force
    Remove-Item -LiteralPath $bbFullUvoptxBak -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $bbFullInit -Force -ErrorAction SilentlyContinue
    Stop-AllTestUv4   # only OUR test instances -- never the user's own uVision
}

$sw.Stop()
$bad = @($script:checks | Where-Object { -not $_.Ok }).Count
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("=== B.4.4.5 clean-environment / end-to-end QA (spec 74-98) ===")
$lines.Add(("package : {0}" -f $Pkg))
$lines.Add(("temp    : {0}" -f $TempRoot))
$lines.Add(("finished: {0}  ({1:N1}s)" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $sw.Elapsed.TotalSeconds))
$lines.Add("")
foreach ($c in $script:checks) {
    $st = if ($c.Ok) { 'ok  ' } else { 'FAIL' }
    $lines.Add(("  {0} {1} {2}" -f $st, $c.Name, $c.Detail))
}
$lines.Add("")
$lines.Add(("result: {0}/{1} checks passed" -f ($script:checks.Count - $bad), $script:checks.Count))
$lines | Set-Content -LiteralPath $report -Encoding UTF8
Write-Host ""
Write-Host ("=== cleanenv QA: {0}/{1} checks passed, {2} failed ===" -f ($script:checks.Count - $bad), $script:checks.Count, $bad) `
           -ForegroundColor $(if ($bad -eq 0) { 'Green' } else { 'Red' })
Write-Host ("report: {0}" -f $report)
if ($bad -gt 0) { exit 1 } else { exit 0 }