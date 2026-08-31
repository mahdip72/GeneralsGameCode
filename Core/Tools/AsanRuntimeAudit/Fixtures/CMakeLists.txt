cmake_minimum_required(VERSION 3.25)

project(asan_runtime_packaging_fixture NONE)

if(NOT DEFINED RTS_ASAN_MODULE OR "${RTS_ASAN_MODULE}" STREQUAL "")
    set(RTS_ASAN_MODULE
        "${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/asan.cmake")
endif()
if(NOT DEFINED RTS_TEST_COMPILER OR "${RTS_TEST_COMPILER}" STREQUAL "")
    message(FATAL_ERROR "RTS_TEST_COMPILER is required.")
endif()

# Emulate the x64 MSVC/ASan facts supplied by the real product configure.
if(NOT DEFINED RTS_BUILD_OPTION_ASAN)
    set(RTS_BUILD_OPTION_ASAN ON)
endif()
set(MSVC TRUE)
set(CMAKE_SIZEOF_VOID_P 8)
set(CMAKE_CXX_COMPILER "${RTS_TEST_COMPILER}")
include("${RTS_ASAN_MODULE}")

if(RTS_BUILD_OPTION_ASAN AND NOT RTS_NATIVE_ASAN_RUNTIME_DLLS)
    message(FATAL_ERROR "The fixture did not resolve the ASan runtime DLL.")
endif()

if(NOT RTS_BUILD_OPTION_ASAN)
    rts_install_asan_runtime("NativeProduct")
    return()
endif()

set(_core_output_directory "${CMAKE_CURRENT_BINARY_DIR}/Core")
rts_stage_asan_runtime("${_core_output_directory}")
if(CMAKE_CONFIGURATION_TYPES)
    file(WRITE "${_core_output_directory}/Debug/core_job_system_tests.exe"
        "ASan executable packaging fixture\n")
    file(WRITE "${_core_output_directory}/Release/core_job_system_tests.exe"
        "ASan release executable packaging fixture\n")
    set(_fixture_executable "${_core_output_directory}/Debug/core_job_system_tests.exe")
else()
    file(WRITE "${_core_output_directory}/core_job_system_tests.exe"
        "ASan single-config executable packaging fixture\n")
    set(_fixture_executable "${_core_output_directory}/core_job_system_tests.exe")
endif()

rts_install_asan_runtime("NativeProduct")
install(FILES "${_fixture_executable}"
    DESTINATION NativeProduct
    CONFIGURATIONS Debug)
