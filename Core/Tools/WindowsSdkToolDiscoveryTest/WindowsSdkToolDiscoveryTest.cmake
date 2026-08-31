cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED RTS_SOURCE_ROOT)
    message(FATAL_ERROR "RTS_SOURCE_ROOT is required.")
endif()

list(APPEND CMAKE_MODULE_PATH "${RTS_SOURCE_ROOT}/cmake")
include(windows-sdk-tools)

# Prove SDK discovery does not depend on a Developer Command Prompt placing
# the shader compiler on PATH.
set(ENV{PATH} "")
unset(RTS_TEST_FXC_EXECUTABLE CACHE)
rts_find_windows_sdk_program(RTS_TEST_FXC_EXECUTABLE NAMES fxc.exe fxc)

if(NOT EXISTS "${RTS_TEST_FXC_EXECUTABLE}")
    message(FATAL_ERROR
        "Windows SDK shader compiler was not resolved: '${RTS_TEST_FXC_EXECUTABLE}'.")
endif()

message(STATUS "Resolved Windows SDK shader compiler: ${RTS_TEST_FXC_EXECUTABLE}")
