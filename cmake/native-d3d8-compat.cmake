# Native Direct3D 8 compatibility bridge used only while the D3D11 backend
# still consumes legacy WW3D resources and state. The product executable does
# not import D3D8 or D3D9; it loads this isolated app-local module explicitly.

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    return()
endif()
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
    message(FATAL_ERROR
        "Native D3D8 compatibility supports AMD64/x64 only; ${CMAKE_SYSTEM_PROCESSOR} is unsupported.")
endif()

find_package(Git REQUIRED)

FetchContent_Declare(
    native_d3d8_compat
    GIT_REPOSITORY https://github.com/crosire/d3d8to9.git
    GIT_TAG        65870f2302e9c496cd6d873d6095961d5c777668
    PATCH_COMMAND
        "${CMAKE_COMMAND}"
        "-DPATCH_GIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DPATCH_SOURCE_DIR=<SOURCE_DIR>"
        "-DPATCH_FILE=${CMAKE_SOURCE_DIR}/cmake/patches/d3d8to9-x64-opaque-handles.patch"
        -DPATCH_SENTINEL_COUNT=3
        "-DPATCH_SENTINEL_FILE_1=source/d3d8to9.cpp"
        "-DPATCH_SENTINEL_TEXT_1=D3D8TO9_NO_MISSING_D3DX_PROMPT"
        "-DPATCH_SENTINEL_FILE_2=source/d3d8to9.hpp"
        "-DPATCH_SENTINEL_TEXT_2=NextPixelShaderHandle = 0x40000001u"
        "-DPATCH_SENTINEL_FILE_3=source/d3d8to9_device.cpp"
        "-DPATCH_SENTINEL_TEXT_3=HasDependentTextureRead"
        -P "${CMAKE_SOURCE_DIR}/cmake/ApplyGitPatch.cmake"
        COMMAND "${CMAKE_COMMAND}"
        "-DPATCH_GIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DPATCH_SOURCE_DIR=<SOURCE_DIR>"
        "-DPATCH_FILE=${CMAKE_SOURCE_DIR}/cmake/patches/d3d8to9-runtime-hardening.patch"
        -DPATCH_SENTINEL_COUNT=4
        "-DPATCH_SENTINEL_FILE_1=source/d3d8to9.cpp"
        "-DPATCH_SENTINEL_TEXT_1=D3DXObjectCount"
        "-DPATCH_SENTINEL_FILE_2=source/d3d8to9.hpp"
        "-DPATCH_SENTINEL_TEXT_2=ReleaseAppLocalD3DX"
        "-DPATCH_SENTINEL_FILE_3=source/d3d8to9_base.cpp"
        "-DPATCH_SENTINEL_TEXT_3=D3DCREATE_MULTITHREADED"
        "-DPATCH_SENTINEL_FILE_4=source/d3d8to9_device.cpp"
        "-DPATCH_SENTINEL_TEXT_4=D3D9 requires state blocks"
        -P "${CMAKE_SOURCE_DIR}/cmake/ApplyGitPatch.cmake"
        COMMAND "${CMAKE_COMMAND}"
        "-DPATCH_GIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DPATCH_SOURCE_DIR=<SOURCE_DIR>"
        "-DPATCH_FILE=${CMAKE_SOURCE_DIR}/cmake/patches/d3d8to9-app-local-compiler.patch"
        -DPATCH_SENTINEL_COUNT=1
        "-DPATCH_SENTINEL_FILE_1=source/d3d8to9.cpp"
        "-DPATCH_SENTINEL_TEXT_1=D3DCompilerModule = LoadAppLocalRuntime"
        -P "${CMAKE_SOURCE_DIR}/cmake/ApplyGitPatch.cmake"
        COMMAND "${CMAKE_COMMAND}"
        "-DPATCH_GIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DPATCH_SOURCE_DIR=<SOURCE_DIR>"
        "-DPATCH_FILE=${CMAKE_SOURCE_DIR}/cmake/patches/d3d8to9-relwithdebinfo-release-crt.patch"
        -DPATCH_SENTINEL_COUNT=1
        "-DPATCH_SENTINEL_FILE_1=CMakeLists.txt"
        "-DPATCH_SENTINEL_TEXT_1=D3D8TO9_RELWITHDEBINFO_RELEASE_CRT"
        -P "${CMAKE_SOURCE_DIR}/cmake/ApplyGitPatch.cmake"
)

set(D3D8TO9_STATIC OFF CACHE BOOL "Build the native D3D8 bridge as an app-local DLL" FORCE)
FetchContent_MakeAvailable(native_d3d8_compat)

set_target_properties(d3d8to9 PROPERTIES
    OUTPUT_NAME d3d8
    FOLDER "Dependencies"
)
target_compile_definitions(d3d8to9 PRIVATE D3D8TO9_NO_MISSING_D3DX_PROMPT)
if(MSVC)
    target_compile_options(d3d8to9 PRIVATE /EHsc /we4302 /we4311 /we4312)
endif()

add_library(rts_native_d3d8_compat_boundary INTERFACE)
add_dependencies(rts_native_d3d8_compat_boundary d3d8to9)

# d3d8to9 translates the game's legacy shader bytecode with D3DX9. Use the
# official Microsoft app-local package rather than requiring DirectSetup or a
# machine-wide legacy SDK installation.
FetchContent_Declare(
    native_d3dx_runtime
    URL https://www.nuget.org/api/v2/package/Microsoft.DXSDK.D3DX/9.29.952.8
    URL_HASH SHA256=EAD0906AE8A26C18A7525DA7490127A2110F7C58F18293738283E30E97C6EA4B
)
FetchContent_MakeAvailable(native_d3dx_runtime)

set(RTS_NATIVE_D3D8_COMPAT_TARGET d3d8to9 CACHE INTERNAL
    "Native app-local D3D8 compatibility target")
set(RTS_NATIVE_D3D8_COMPAT_DLL "$<TARGET_FILE:d3d8to9>" CACHE INTERNAL
    "Native app-local D3D8 compatibility module")
set(RTS_NATIVE_D3D8_COMPAT_LICENSE "${native_d3d8_compat_SOURCE_DIR}/LICENSE.md"
    CACHE INTERNAL "Native D3D8 compatibility license")
set(RTS_NATIVE_D3DX_RUNTIME_DLLS
    "${native_d3dx_runtime_SOURCE_DIR}/build/native/release/bin/x64/D3DCompiler_43.dll"
    "${native_d3dx_runtime_SOURCE_DIR}/build/native/release/bin/x64/D3DX9_43.dll"
    CACHE INTERNAL "App-local D3DX runtime files for native D3D8 compatibility")
add_custom_target(rts_native_d3dx_stage ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${RTS_NATIVE_D3DX_RUNTIME_DLLS} "$<TARGET_FILE_DIR:d3d8to9>"
    COMMENT "Staging app-local D3DX runtimes beside d3d8.dll")
add_dependencies(rts_native_d3dx_stage d3d8to9)
set(RTS_NATIVE_D3DX_LICENSE_FILES
    "${native_d3dx_runtime_SOURCE_DIR}/LICENSE.txt"
    "${native_d3dx_runtime_SOURCE_DIR}/NOTICE.md"
    CACHE INTERNAL "Microsoft D3DX runtime notices")

foreach(_runtime IN LISTS RTS_NATIVE_D3DX_RUNTIME_DLLS RTS_NATIVE_D3DX_LICENSE_FILES)
    if(NOT EXISTS "${_runtime}")
        message(FATAL_ERROR "Native D3D8 compatibility runtime file is missing: ${_runtime}")
    endif()
endforeach()
