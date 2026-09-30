cmake_minimum_required(VERSION 3.25)

include("${CMAKE_CURRENT_LIST_DIR}/native-target-architecture.cmake")

function(_assert_target_architecture_case case_name should_pass)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DRTS_NATIVE_TARGET_ARCHITECTURE_CASE=${case_name}"
            -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)
    set(_output "${_stdout}\n${_stderr}")

    if(should_pass)
        if(NOT "${_result}" STREQUAL "0")
            message(FATAL_ERROR
                "Target architecture case '${case_name}' should pass but failed: ${_output}")
        endif()
    else()
        if("${_result}" STREQUAL "0")
            message(FATAL_ERROR
                "Target architecture case '${case_name}' should fail but passed.")
        endif()
        if(NOT _output MATCHES "requires an AMD64/x64")
            message(FATAL_ERROR
                "Target architecture case '${case_name}' failed for an unexpected reason: ${_output}")
        endif()
    endif()

    message(STATUS "Passed target architecture case '${case_name}'.")
endfunction()

if(NOT DEFINED RTS_NATIVE_TARGET_ARCHITECTURE_CASE OR
        "${RTS_NATIVE_TARGET_ARCHITECTURE_CASE}" STREQUAL "")
    _assert_target_architecture_case(current-ninja-x64 TRUE)
    _assert_target_architecture_case(visual-studio-x64 TRUE)
    _assert_target_architecture_case(x64-cross-target-arm64-host TRUE)
    _assert_target_architecture_case(system-processor-x64-fallback TRUE)
    _assert_target_architecture_case(arm64 FALSE)
    _assert_target_architecture_case(missing-target-signals FALSE)
    _assert_target_architecture_case(unknown-processor FALSE)
    _assert_target_architecture_case(ninja-ignored-platform FALSE)
    _assert_target_architecture_case(unknown-authoritative-signal FALSE)
    _assert_target_architecture_case(conflicting-authoritative-signals FALSE)
    message(STATUS "Native target architecture script tests passed.")
    return()
endif()

foreach(_variable IN ITEMS
        CMAKE_GENERATOR
        CMAKE_GENERATOR_PLATFORM
        CMAKE_VS_PLATFORM_NAME
        CMAKE_C_COMPILER_ARCHITECTURE_ID
        CMAKE_CXX_COMPILER_ARCHITECTURE_ID
        CMAKE_C_COMPILER_TARGET
        CMAKE_CXX_COMPILER_TARGET
        CMAKE_SYSTEM_PROCESSOR
        CMAKE_HOST_SYSTEM_PROCESSOR)
    set(${_variable} "")
endforeach()

if(RTS_NATIVE_TARGET_ARCHITECTURE_CASE STREQUAL "current-ninja-x64")
    set(CMAKE_GENERATOR "Ninja Multi-Config")
    set(CMAKE_C_COMPILER_ARCHITECTURE_ID "x64")
    set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID "x64")
    set(CMAKE_SYSTEM_PROCESSOR "AMD64")
elseif(RTS_NATIVE_TARGET_ARCHITECTURE_CASE STREQUAL "visual-studio-x64")
    set(CMAKE_GENERATOR "Visual Studio 17 2022")
    set(CMAKE_GENERATOR_PLATFORM "x64")
    set(CMAKE_VS_PLATFORM_NAME "x64")
    set(CMAKE_C_COMPILER_ARCHITECTURE_ID "x64")
    set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID "x64")
    set(CMAKE_C_COMPILER_TARGET "x86_64-pc-windows-msvc")
    set(CMAKE_CXX_COMPILER_TARGET "x86_64-pc-windows-msvc")
    set(CMAKE_SYSTEM_PROCESSOR "AMD64")
elseif(RTS_NATIVE_TARGET_ARCHITECTURE_CASE STREQUAL "x64-cross-target-arm64-host")
    set(CMAKE_GENERATOR "Ninja")
    set(CMAKE_C_COMPILER_TARGET "x86_64-pc-windows-msvc")
    set(CMAKE_CXX_COMPILER_TARGET "x86_64-pc-windows-msvc")
    set(CMAKE_SYSTEM_PROCESSOR "ARM64")
    set(CMAKE_HOST_SYSTEM_PROCESSOR "ARM64")
elseif(RTS_NATIVE_TARGET_ARCHITECTURE_CASE STREQUAL "system-processor-x64-fallback")
    set(CMAKE_GENERATOR "Ninja")
    set(CMAKE_SYSTEM_PROCESSOR "AMD64")
elseif(RTS_NATIVE_TARGET_ARCHITECTURE_CASE STREQUAL "arm64")
    set(CMAKE_GENERATOR "Visual Studio 17 2022")
    set(CMAKE_GENERATOR_PLATFORM "ARM64")
    set(CMAKE_VS_PLATFORM_NAME "ARM64")
    set(CMAKE_C_COMPILER_ARCHITECTURE_ID "ARM64")
    set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID "ARM64")
    set(CMAKE_C_COMPILER_TARGET "aarch64-pc-windows-msvc")
    set(CMAKE_CXX_COMPILER_TARGET "aarch64-pc-windows-msvc")
    set(CMAKE_SYSTEM_PROCESSOR "ARM64")
elseif(RTS_NATIVE_TARGET_ARCHITECTURE_CASE STREQUAL "missing-target-signals")
    set(CMAKE_HOST_SYSTEM_PROCESSOR "AMD64")
elseif(RTS_NATIVE_TARGET_ARCHITECTURE_CASE STREQUAL "unknown-processor")
    set(CMAKE_SYSTEM_PROCESSOR "mystery64")
elseif(RTS_NATIVE_TARGET_ARCHITECTURE_CASE STREQUAL "ninja-ignored-platform")
    set(CMAKE_GENERATOR "Ninja")
    set(CMAKE_GENERATOR_PLATFORM "x64")
    set(CMAKE_SYSTEM_PROCESSOR "ARM64")
elseif(RTS_NATIVE_TARGET_ARCHITECTURE_CASE STREQUAL "unknown-authoritative-signal")
    set(CMAKE_GENERATOR "Visual Studio 17 2022")
    set(CMAKE_GENERATOR_PLATFORM "x64")
    set(CMAKE_CXX_COMPILER_TARGET "mystery64-pc-windows-msvc")
    set(CMAKE_SYSTEM_PROCESSOR "AMD64")
elseif(RTS_NATIVE_TARGET_ARCHITECTURE_CASE STREQUAL "conflicting-authoritative-signals")
    set(CMAKE_GENERATOR "Visual Studio 17 2022")
    set(CMAKE_GENERATOR_PLATFORM "x64")
    set(CMAKE_VS_PLATFORM_NAME "x64")
    set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID "ARM64")
    set(CMAKE_CXX_COMPILER_TARGET "x86_64-pc-windows-msvc")
    set(CMAKE_SYSTEM_PROCESSOR "AMD64")
else()
    message(FATAL_ERROR
        "Unknown RTS_NATIVE_TARGET_ARCHITECTURE_CASE: ${RTS_NATIVE_TARGET_ARCHITECTURE_CASE}")
endif()

rts_require_native_amd64_target("Architecture test '${RTS_NATIVE_TARGET_ARCHITECTURE_CASE}'")
