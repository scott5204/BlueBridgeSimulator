@echo off
rem Build the debug-target test firmware with the STM32CubeCLT GNU ARM toolchain.
rem Output: test_debug.hex / test_debug.elf / test_debug.map (this directory)
setlocal
where arm-none-eabi-gcc >nul 2>nul
if errorlevel 1 (
  set "GCC=C:\ST\STM32CubeCLT_1.19.0\GNU-tools-for-STM32\bin\arm-none-eabi-gcc.exe"
) else (
  set GCC=arm-none-eabi-gcc
)
set OBJCOPY=%GCC:arm-none-eabi-gcc.exe=arm-none-eabi-objcopy.exe%
if not exist "%OBJCOPY%" set OBJCOPY=arm-none-eabi-objcopy

set COMMON=-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -O2 -ffunction-sections -fdata-sections -ffreestanding -Wall

cd /d "%~dp0"

"%GCC%" %COMMON% -c main.s -o main.o || goto :err

"%GCC%" -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -nostdlib ^
  -T linker.ld -Wl,--gc-sections -Wl,-Map=test_debug.map ^
  main.o -lgcc -o test_debug.elf || goto :err

"%OBJCOPY%" -O ihex test_debug.elf test_debug.hex || goto :err

del main.o
echo === test_debug.hex built ===
dir /b test_debug.hex
exit /b 0

:err
echo BUILD FAILED
exit /b 1