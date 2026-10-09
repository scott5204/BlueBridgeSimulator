@echo off
rem Build the ADC/PWM closed-loop firmware (ADC -> PWM duty/frequency,
rem PA15 capture -> frequency comparison -> ALARM).
rem The official CT117E LCD BSP (lcd.c/lcd.h/fonts.h) is compiled verbatim;
rem hal_shim.c provides the small HAL subset the BSP needs.
rem Output: test_adc_pwm.hex / test_adc_pwm.elf / test_adc_pwm.map
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

"%GCC%" %COMMON% -c startup.c -o startup.o || goto :err
"%GCC%" %COMMON% -c main.c -o main.o || goto :err
"%GCC%" %COMMON% -c hal_shim.c -o hal_shim.o || goto :err
"%GCC%" %COMMON% -c lcd.c -o lcd.o || goto :err

"%GCC%" -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -nostdlib ^
  -T linker.ld -Wl,--gc-sections -Wl,-Map=test_adc_pwm.map ^
  startup.o main.o hal_shim.o lcd.o -lgcc -o test_adc_pwm.elf || goto :err

"%OBJCOPY%" -O ihex test_adc_pwm.elf test_adc_pwm.hex || goto :err

del startup.o main.o hal_shim.o lcd.o
echo === test_adc_pwm.hex built ===
dir /b test_adc_pwm.hex
exit /b 0

:err
echo BUILD FAILED
exit /b 1