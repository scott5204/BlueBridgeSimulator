cmake_minimum_required(VERSION 3.21)

project(BlueBridgeSimulator LANGUAGES C CXX)

# Product version (Windows version resource + packaging; spec B.4.4 section 40).
# The packaging script passes -DAPP_VERSION=<VERSION.txt>; the cache default
# keeps a plain `cmake -S . -B build` working.
set(APP_VERSION "1.0.0" CACHE STRING "BlueBridgeSimulator product version")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    enable_language(RC)
endif()

# Always emit debug info (even in Release): the sim occasionally has to be
# analysed by attaching gdb to a live process (a GUI freeze), and without -g the
# backtrace has no line numbers/locals -- function names alone were not enough
# to pin the 2026-09-26 freeze to a single loop. Costs binary size only.
#
# The dev-machine absolute paths are remapped to stable relative prefixes so the
# shipped binary carries no developer directory names (spec B.4.4 section 113)
# while the file:line information stays usable for post-mortem analysis.
add_compile_options(-g)
add_compile_options(-ffile-prefix-map=${CMAKE_SOURCE_DIR}=BlueBridgeSimulator)
add_compile_options(-ffile-prefix-map=${CMAKE_BINARY_DIR}=build)

if(WIN32)
    add_definitions(-DUNICODE -D_UNICODE)
endif()

# Qt6 is located via CMAKE_PREFIX_PATH (see README.md / scripts/configure.cmd)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets)

# NOTE: a plain `set(CMAKE_AUTOM ON)` is silently ignored by the CMake builds
# available on this machine (verified empirically); qt_standard_project_setup()
# enables moc processing correctly.
qt_standard_project_setup()

set(UNICORN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/unicorn)

# ----------------------------------------------------------------------------
# Core simulation library (no GUI dependencies)
# ----------------------------------------------------------------------------
add_library(bluesim_core STATIC
    src/core/MemoryBus.cpp
    src/loader/HexLoader.cpp
    src/cpu/UnicornApi.cpp
    src/cpu/UnicornCpu.cpp
    src/stm32/Stm32G431.cpp
    src/stm32/rcc/Rcc.cpp
    src/stm32/gpio/GpioPort.cpp
    src/stm32/Ppb.cpp
    src/stm32/tim/Timer.cpp
    src/stm32/adc/Adc.cpp
    src/stm32/usart/Usart.cpp
    src/board/Ct117eM4.cpp
    src/board/lcd/LcdController.cpp
    src/board/analog/AnalogSource.cpp
    src/board/signal/PulseInputSource.cpp
    src/board/signal/DigitalSignalMonitor.cpp
    src/sim/Simulator.cpp
    src/debug/SimulatorDebugTarget.cpp
    src/debug/SimulatorFlashProgrammer.cpp
    src/debug/ipc/DebugIpcCodec.cpp
    src/debug/ipc/DebugCommandQueue.cpp
    src/debug/ipc/DebugIpcServer.cpp
)
target_include_directories(bluesim_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${UNICORN_DIR}/include
)
target_link_libraries(bluesim_core PUBLIC Qt6::Core)

# ----------------------------------------------------------------------------
# GUI application
# ----------------------------------------------------------------------------
set(BLUESIM_SOURCES
    src/main.cpp
    src/gui/MainWindow.cpp
    src/gui/LedIndicator.cpp
    src/gui/LcdWidget.cpp
)
if(WIN32)
    # Windows version resource (spec B.4.4 section 41): generated from
    # src/win/app.rc.in so APP_VERSION stays the single source of truth.
    set(_parts ${APP_VERSION})
    string(REPLACE "." ";" _parts "${APP_VERSION}")
    list(LENGTH _parts _n)
    while(_n LESS 4)
        list(APPEND _parts 0)
        math(EXPR _n "${_n} + 1")
    endwhile()
    list(JOIN _parts "," APP_VER_COMMA)
    configure_file(src/win/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/app.rc @ONLY)
    list(APPEND BLUESIM_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/app.rc)
endif()

add_executable(bluesim WIN32 ${BLUESIM_SOURCES})
target_link_libraries(bluesim PRIVATE bluesim_core Qt6::Widgets)

# ----------------------------------------------------------------------------
# Tests
# ----------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)

# ----------------------------------------------------------------------------
# Copy unicorn.dll next to the GUI executable so it can be loaded at runtime
# ----------------------------------------------------------------------------
add_custom_command(TARGET bluesim POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${UNICORN_DIR}/bin/unicorn.dll $<TARGET_FILE_DIR:bluesim>)
