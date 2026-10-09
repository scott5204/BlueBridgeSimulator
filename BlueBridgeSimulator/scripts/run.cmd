@echo off
rem Run the simulator GUI with the correct DLL paths on PATH.
rem Override QTDIR / MINGW when the tools live elsewhere (defaults = the
rem reference development machine's paths).
setlocal
set "ROOT=%~dp0.."
if not defined QTDIR set "QTDIR=D:\STM32\lqbfz\tools\Qt\6.8.3\mingw_64"
if not defined MINGW set "MINGW=D:\STM32\lqbfz\tools\Qt\Tools\mingw1310_64\bin"
set "PATH=%QTDIR%\bin;%MINGW%;%PATH%"
start "" "%ROOT%\build\bluesim.exe" %*
