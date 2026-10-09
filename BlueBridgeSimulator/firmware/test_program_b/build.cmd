@echo off
rem Build the virtual-flash programming test firmware, build B (stage 7-2A).
rem Output: test_program_b.hex / .elf / .map (this directory)
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
  -T linker.ld -Wl,--gc-sections -Wl,-Map=test_program_b.map ^
  main.o -lgcc -o test_program_b.elf || goto :err

"%OBJCOPY%" -O ihex test_program_b.elf test_program_b.hex || goto :err

del main.o
echo === test_program_b.hex built ===
dir /b test_program_b.hex
exit /b 0

:err
echo BUILD FAILED
exit /b 1
