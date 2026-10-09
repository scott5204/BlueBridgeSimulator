cmake_minimum_required(VERSION 3.21)

# Unit test: Intel HEX / BIN loader
add_executable(test_hexloader test_hexloader.cpp)
target_link_libraries(test_hexloader PRIVATE bluesim_core)
add_test(NAME hexloader COMMAND test_hexloader)

# Unit test: timer register file, counting, PWM output, input capture
add_executable(test_timer test_timer.cpp)
target_link_libraries(test_timer PRIVATE bluesim_core)
add_test(NAME timer COMMAND test_timer)

# Unit test: ADC register file, HAL-compatible sequence, resolution/alignment,
# EOC/OVR semantics, board-supplied channel voltages
add_executable(test_adc test_adc.cpp)
target_link_libraries(test_adc PRIVATE bluesim_core)
add_test(NAME adc COMMAND test_adc)

# Headless simulation self test: runs the ARM test firmware on the emulated
# Cortex-M4 and verifies LED blinking, SysTick timing and the button path.
add_executable(sim_selftest sim_selftest.cpp)
target_link_libraries(sim_selftest PRIVATE bluesim_core)
add_test(NAME sim_selftest COMMAND sim_selftest
    ${CMAKE_SOURCE_DIR}/firmware/test_blink/test_blink.hex)
add_custom_command(TARGET sim_selftest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/third_party/unicorn/bin/unicorn.dll
            $<TARGET_FILE_DIR:sim_selftest>)

# Headless TIM test: runs the timer test firmware (bare metal, real MMIO) and
# verifies base timer interrupts, PWM at the PA7 pin, dynamic duty, input
# capture from the virtual generator, wrong-AF and CEN=0 behaviour.
add_executable(tim_selftest tim_selftest.cpp)
target_link_libraries(tim_selftest PRIVATE bluesim_core)
add_test(NAME tim_selftest COMMAND tim_selftest
    ${CMAKE_SOURCE_DIR}/firmware/test_tim/test_tim.hex)
add_custom_command(TARGET tim_selftest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/third_party/unicorn/bin/unicorn.dll
            $<TARGET_FILE_DIR:tim_selftest>)

# Headless stage-4 test: PA15 pulse input (firmware capture), R37/R38 ADC
# channels, PA7 PWM measured at the pin, wrong-AF and the closed loop.
add_executable(analog_selftest analog_selftest.cpp)
target_link_libraries(analog_selftest PRIVATE bluesim_core)
add_test(NAME analog_selftest COMMAND analog_selftest
    ${CMAKE_SOURCE_DIR}/firmware/test_adc_pwm/test_adc_pwm.hex)
add_custom_command(TARGET analog_selftest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/third_party/unicorn/bin/unicorn.dll
            $<TARGET_FILE_DIR:analog_selftest>)


# Headless USART test (stage 5): the USART1 model at register/timing level plus

# the DAP-Link serial firmware -- TX/RX, the RXNE interrupt, byte timing at two
# baud rates, wrong-AF both ways, the overrun flag and the line protocol, all
# driven through the board's virtual PC terminal.
add_executable(usart_selftest usart_selftest.cpp)
target_link_libraries(usart_selftest PRIVATE bluesim_core)
add_test(NAME usart_selftest COMMAND usart_selftest
    ${CMAKE_SOURCE_DIR}/firmware/test_usart/test_usart.hex)
add_custom_command(TARGET usart_selftest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/third_party/unicorn/bin/unicorn.dll
            $<TARGET_FILE_DIR:usart_selftest>)

# Regression: loading a second firmware after running one (unicorn must drop the
# first firmware's translated code, and loading must auto-pause the run).
add_executable(reload_selftest reload_selftest.cpp)
target_link_libraries(reload_selftest PRIVATE bluesim_core)
add_test(NAME reload_selftest COMMAND reload_selftest
    ${CMAKE_SOURCE_DIR}/firmware/)
add_custom_command(TARGET reload_selftest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/third_party/unicorn/bin/unicorn.dll
            $<TARGET_FILE_DIR:reload_selftest>)

# Headless acceptance test for firmware/test_full (the firmware the user drives
# by hand): LED chaser, key presses (including a 12 ms short click -- a 20 ms
# debounce used to eat half of them), key actions, PA7 PWM preset, PA15/PB4
# capture and the three LCD pages.
add_executable(full_selftest full_selftest.cpp)
target_link_libraries(full_selftest PRIVATE bluesim_core)
add_test(NAME full_selftest COMMAND full_selftest
    ${CMAKE_SOURCE_DIR}/firmware/test_full/test_full.hex)
add_custom_command(TARGET full_selftest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/third_party/unicorn/bin/unicorn.dll
            $<TARGET_FILE_DIR:full_selftest>)

# Headless stage-7-1 acceptance test: the Virtual Debug Target Core
# (src/debug/) against firmware/test_debug -- reset-halt, register/memory
# access, single step, breakpoints (incl. a 100000x hit/continue stress loop),
# halt/resume real-time semantics and the existing-firmware regressions.
add_executable(debug_target_selftest debug_target_selftest.cpp)
target_link_libraries(debug_target_selftest PRIVATE bluesim_core)
add_test(NAME debug_target_selftest COMMAND debug_target_selftest
    ${CMAKE_SOURCE_DIR}/firmware/)
add_custom_command(TARGET debug_target_selftest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/third_party/unicorn/bin/unicorn.dll
            $<TARGET_FILE_DIR:debug_target_selftest>)

# Headless stage-7-2d acceptance test: exact debugger single-stepping on the
# real unicorn.dll (UC_CTL_EXACT_SINGLE_STEP). 100000 single steps are run for
# straight-line code, loops, taken/not-taken branches, BL/BX LR, Thumb-2 mix,
# MMIO, handler mode and exception return -- every step must execute EXACTLY
# one guest instruction. Also checks that the normal (non-debug) batch path
# still executes multi-instruction blocks.
add_executable(exact_step_selftest exact_step_selftest.cpp)
target_link_libraries(exact_step_selftest PRIVATE bluesim_core)
add_test(NAME exact_step_selftest COMMAND exact_step_selftest)
add_custom_command(TARGET exact_step_selftest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/third_party/unicorn/bin/unicorn.dll
            $<TARGET_FILE_DIR:exact_step_selftest>)

# Headless stage-7-2A acceptance test: Debug IPC (Windows Named Pipe) + virtual
# flash programming over IPC. The client runs on its own thread and exchanges
# real pipe bytes; the main thread is the simulator owner (command pump +
# runTick), so the owner-thread contract of stage 7-1 is really exercised.
add_executable(debug_ipc_selftest debug_ipc_selftest.cpp)
target_link_libraries(debug_ipc_selftest PRIVATE bluesim_core)
add_test(NAME debug_ipc_selftest COMMAND debug_ipc_selftest
    ${CMAKE_SOURCE_DIR}/firmware/)
add_custom_command(TARGET debug_ipc_selftest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/third_party/unicorn/bin/unicorn.dll
            $<TARGET_FILE_DIR:debug_ipc_selftest>)

# Headless stage-7-2A: virtual flash programming, DIRECT (no IPC) --
# IFlashProgrammer staging / abort / range errors, atomic commit, alias mirror,
# 128 KiB images and A->B->A x100 with the stale-TCG hard check.
add_executable(debug_program_selftest debug_program_selftest.cpp)
target_link_libraries(debug_program_selftest PRIVATE bluesim_core)
add_test(NAME debug_program_selftest COMMAND debug_program_selftest
    ${CMAKE_SOURCE_DIR}/firmware/)
add_custom_command(TARGET debug_program_selftest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/third_party/unicorn/bin/unicorn.dll
            $<TARGET_FILE_DIR:debug_program_selftest>)

# Stage-7-2A wire codec golden test: fixed 24-byte little-endian header, frozen
# enum/register values, bounds-safe payload reader (no unicorn/Qt needed).
add_executable(debug_ipc_codec_test debug_ipc_codec_test.cpp)
target_link_libraries(debug_ipc_codec_test PRIVATE bluesim_core)
add_test(NAME debug_ipc_codec_test COMMAND debug_ipc_codec_test)

# Headless LCD test: phase A checks the LcdController bus/GRAM model at pin
# level, phase B runs the LCD test firmware (official BSP) and verifies the
# framebuffer content that the firmware produced over the GPIO bus, phase C
# renders the real GUI widget (gui/LcdWidget) offscreen and checks the
# RGB565 conversion / integer scaling.
add_executable(lcd_selftest lcd_selftest.cpp ${CMAKE_SOURCE_DIR}/src/gui/LcdWidget.cpp)
target_link_libraries(lcd_selftest PRIVATE bluesim_core Qt6::Widgets)
add_test(NAME lcd_selftest COMMAND lcd_selftest
    ${CMAKE_SOURCE_DIR}/firmware/test_lcd/test_lcd.hex)
add_custom_command(TARGET lcd_selftest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/third_party/unicorn/bin/unicorn.dll
            $<TARGET_FILE_DIR:lcd_selftest>)
# Firmware bring-up helper (not a pass/fail test): runs any firmware image for
# a chosen amount of virtual time and dumps the LCD panel + board state.
add_executable(fw_shot fw_shot.cpp)
target_link_libraries(fw_shot PRIVATE bluesim_core Qt6::Gui)
add_custom_command(TARGET fw_shot POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/third_party/unicorn/bin/unicorn.dll
            $<TARGET_FILE_DIR:fw_shot>)

# Qt platform plugins so the offscreen widget render (phase C) works from the
# build tree. Qt6_DIR = <qt>/lib/cmake/Qt6, so <qt>/plugins is 3 levels up.
get_filename_component(_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
set(_qt_platform_plugins "${_qt_prefix}/plugins/platforms")
if(EXISTS "${_qt_platform_plugins}/qwindows.dll")
    foreach(_qt_plugin qwindows qoffscreen)
        add_custom_command(TARGET lcd_selftest POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    $<TARGET_FILE_DIR:lcd_selftest>/platforms
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${_qt_platform_plugins}/${_qt_plugin}.dll
                    $<TARGET_FILE_DIR:lcd_selftest>/platforms/)
    endforeach()
else()
    message(WARNING "Qt platform plugins not found at ${_qt_platform_plugins}; "
                    "lcd_selftest phase C needs them at runtime")
endif()
