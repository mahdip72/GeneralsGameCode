cmake_minimum_required(VERSION 3.25)

# Use packagename_ROOT for FindPackage.
if(POLICY CMP0074)
    cmake_policy(SET CMP0074 NEW)
endif()

# Disable default MSVC setting CRT type so we can set it ourselves.
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

# Disable default MSVC warning level so we can set it ourselves.
if(POLICY CMP0092)
    cmake_policy(SET CMP0092 NEW)
endif()

# Allow specifying MSVC debug configurations.
#if(POLICY CMP0141)
#    cmake_policy(SET CMP0141 NEW)
#endif()

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/")

include(FeatureSummary)
include(CMakeDependentOption)

# We don't support in tree builds, so help people make the right choice.
if (CMAKE_BINARY_DIR STREQUAL CMAKE_SOURCE_DIR)
    message(FATAL_ERROR "Building in-source is not supported! Create a build dir and remove ${CMAKE_SOURCE_DIR}/CMakeCache.txt")
endif()

# Top level project, doesn't really affect anything.
project(genzh LANGUAGES C CXX)

include(CTest)

# This file handles extra settings wanted/needed for different compilers.
include(cmake/compilers.cmake)

# Debug symbol stripping for Release builds (MinGW)
include(cmake/debug_strip.cmake)

include(FetchContent)

include(cmake/config.cmake)
include(cmake/asan.cmake)
include(cmake/native-product-packaging.cmake)

# MinGW-w64 specific configuration
if(MINGW)
    include(cmake/mingw.cmake)
    include(cmake/reactos-atl.cmake)
    include(cmake/widl.cmake)
endif()

# Find/Add build dependencies and stubs shared by all projects
if(WIN32 OR "${CMAKE_SYSTEM}" MATCHES "Windows")
	set(_RTS_BUILD_REQUIRES_DX8 OFF)
	if(RTS_BUILD_PRODUCT OR RTS_BUILD_CORE_TOOLS OR RTS_BUILD_CORE_EXTRAS OR
		RTS_BUILD_GENERALS_TOOLS OR RTS_BUILD_GENERALS_EXTRAS OR
		RTS_BUILD_ZEROHOUR_TOOLS OR RTS_BUILD_ZEROHOUR_EXTRAS)
		set(_RTS_BUILD_REQUIRES_DX8 ON)
	endif()
    if(${CMAKE_SIZEOF_VOID_P} EQUAL 4 AND NOT RTS_VIDEO_BACKEND_GRAPH_AUDIT)
        include(cmake/miles.cmake)
    endif()
    if(${CMAKE_SIZEOF_VOID_P} EQUAL 4 AND NOT RTS_BUILD_OPTION_FFMPEG)
        include(cmake/bink.cmake)
    endif()
    if(_RTS_BUILD_REQUIRES_DX8 AND NOT RTS_VIDEO_BACKEND_GRAPH_AUDIT)
        include(cmake/dx8.cmake)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND
            (RTS_BUILD_PRODUCT OR RTS_BUILD_CORE_EXTRAS))
            include(cmake/native-d3d8-compat.cmake)
        endif()
    endif()
endif()

# Define a dummy stlport target when not on VC6.
if (IS_VS6_BUILD)
    include(cmake/stlport.cmake)
else()
    add_library(stlport INTERFACE)
endif()

include(cmake/xaudio2.cmake)
include(cmake/legacy-product-runtime.cmake)
include(cmake/gamespy.cmake)
include(cmake/lzhl.cmake)
include(cmake/stb.cmake)

if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    # Native installed runtimes are disposable and must launch without relying
    # on a machine-wide Visual C++ Redistributable.  Collect both sets because
    # Debug and ASan binaries link the debug CRT and ucrtbased.dll, while the
    # release configurations must retain only the release CRT.
    rts_collect_native_msvc_runtime_dlls()
    set(RTS_NATIVE_MSVC_RUNTIME_DLLS_RELEASE
        "${RTS_NATIVE_MSVC_RUNTIME_DLLS_RELEASE}"
        CACHE INTERNAL "Resolved release MSVC runtime DLLs for native products" FORCE)
    set(RTS_NATIVE_MSVC_RUNTIME_DLLS_DEBUG
        "${RTS_NATIVE_MSVC_RUNTIME_DLLS_DEBUG}"
        CACHE INTERNAL "Resolved debug MSVC runtime DLLs for native products" FORCE)
    # Keep the old cache name as a release-only compatibility alias for tools
    # that have not yet switched to the configuration-specific contract.
    set(RTS_NATIVE_MSVC_RUNTIME_DLLS
        "${RTS_NATIVE_MSVC_RUNTIME_DLLS_RELEASE}"
        CACHE INTERNAL "Resolved release MSVC runtime DLLs for native products" FORCE)
    if(RTS_BUILD_PRODUCT AND
            (NOT RTS_NATIVE_MSVC_RUNTIME_DLLS_RELEASE OR
             NOT RTS_NATIVE_MSVC_RUNTIME_DLLS_DEBUG))
        message(FATAL_ERROR
            "Native x64 product install could not resolve both MSVC runtime sets.")
    endif()
endif()

if (IS_VS6_BUILD)
    # The original max sdk does not compile against a modern compiler.
    # If there is a desire to make this work, then a fixed max sdk needs to be created.
    add_subdirectory(Dependencies/MaxSDK)
endif()
add_subdirectory(Dependencies/Utility)

# To be removed when abandoning VC6
if (IS_VS6_BUILD)
    include_directories(Dependencies/Utility)
    add_compile_options(/FIUtility/CppMacros.h)
endif()

add_subdirectory(resources)

add_subdirectory(Core)

# Add main build targets
if(RTS_BUILD_PRODUCT AND RTS_BUILD_ZEROHOUR)
    add_subdirectory(GeneralsMD)
endif()

if(RTS_BUILD_PRODUCT AND RTS_BUILD_GENERALS)
    add_subdirectory(Generals)
endif()

if(RTS_BUILD_OPTION_ASAN AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_rts_asan_deployment_audit_product_args)
    if(RTS_BUILD_GENERALS)
        list(APPEND _rts_asan_deployment_audit_product_args -ExpectGenerals)
    endif()
    if(RTS_BUILD_ZEROHOUR)
        list(APPEND _rts_asan_deployment_audit_product_args -ExpectZeroHour)
    endif()
    add_test(NAME core_asan_runtime_deployment_audit
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/Core/Tools/AsanRuntimeAudit/AsanRuntimeDeploymentAudit.ps1"
            -BuildRoot "${CMAKE_BINARY_DIR}"
            -RuntimeDll "${RTS_NATIVE_ASAN_RUNTIME_DLLS}"
            ${_rts_asan_deployment_audit_product_args})
    set_tests_properties(core_asan_runtime_deployment_audit PROPERTIES TIMEOUT 30)
endif()

feature_summary(WHAT ENABLED_FEATURES DESCRIPTION "Enabled features:")
feature_summary(WHAT DISABLED_FEATURES DESCRIPTION "Disabled features:")
