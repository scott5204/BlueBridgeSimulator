@echo off
rem ============================================================================
rem Configure + build BlueBridgeSimulator (Windows, Qt 6.8.3 MinGW + Ninja)
rem Usage: scripts\configure.cmd [--debug]
rem Tool paths: override with the environment variables QTDIR / MINGW / NINJA /
rem CMAKE; the defaults below are the reference development machine's.
rem ============================================================================
setlocal enabledelayedexpansion
set "ROOT=%~dp0.."
if not defined QTDIR  set "QTDIR=D:\STM32\lqbfz\tools\Qt\6.8.3\mingw_64"
if not defined MINGW  set "MINGW=D:\STM32\lqbfz\tools\Qt\Tools\mingw1310_64\bin"
if not defined NINJA  set "NINJA=C:\ST\STM32CubeCLT_1.19.0\Ninja\bin"
if not defined CMAKE  set "CMAKE=C:\ST\STM32CubeCLT_1.19.0\CMake\bin\cmake.exe"

set "PATH=%MINGW%;%QTDIR%\bin;%NINJA%;%PATH%"

set BUILD_TYPE=Release
if "%1"=="--debug" set BUILD_TYPE=Debug

"%CMAKE%" -G Ninja -S "%ROOT%" -B "%ROOT%\build" ^
  -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
  -DCMAKE_PREFIX_PATH="%QTDIR%" ^
  -DCMAKE_C_COMPILER="%MINGW%\gcc.exe" ^
  -DCMAKE_CXX_COMPILER="%MINGW%\g++.exe" || exit /b 1

"%CMAKE%" --build "%ROOT%\build" || exit /b 1

echo.
echo === build finished ===
echo binaries: %ROOT%\build\bluesim.exe, %ROOT%\build\tests\*.exe
exit /b 0
