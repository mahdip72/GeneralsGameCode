cmake_minimum_required(VERSION 3.25)

project(native_product_runtime_packaging_fixture NONE)

if(NOT DEFINED RTS_PACKAGING_MODULE OR "${RTS_PACKAGING_MODULE}" STREQUAL "")
    set(RTS_PACKAGING_MODULE
        "${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/native-product-packaging.cmake")
endif()
include("${RTS_PACKAGING_MODULE}")

if(NOT DEFINED RTS_BUILD_OUTPUT_SUFFIX)
    set(RTS_BUILD_OUTPUT_SUFFIX "")
endif()
if(NOT DEFINED RTS_TEST_MSVC_REDIST_DIR OR "${RTS_TEST_MSVC_REDIST_DIR}" STREQUAL "")
    message(FATAL_ERROR "RTS_TEST_MSVC_REDIST_DIR is required.")
endif()
if(NOT DEFINED RTS_TEST_WINDOWS_KITS_DIR OR "${RTS_TEST_WINDOWS_KITS_DIR}" STREQUAL "")
    message(FATAL_ERROR "RTS_TEST_WINDOWS_KITS_DIR is required.")
endif()
if(NOT DEFINED RTS_TEST_WINDOWS_KITS_VERSION OR "${RTS_TEST_WINDOWS_KITS_VERSION}" STREQUAL "")
    message(FATAL_ERROR "RTS_TEST_WINDOWS_KITS_VERSION is required.")
endif()

# This fixture deliberately supplies the compiler facts that the real x64
# project receives from MSVC.  It does not compile anything; it verifies the
# configure/install contract against the installed VS redist files.
set(MSVC TRUE)
set(CMAKE_SIZEOF_VOID_P 8)
set(CMAKE_MSVC_ARCH x64)
set(MSVC_TOOLSET_VERSION 143)
set(MSVC_VERSION 1930)
set(MSVC_REDIST_DIR "${RTS_TEST_MSVC_REDIST_DIR}")
set(WINDOWS_KITS_DIR "${RTS_TEST_WINDOWS_KITS_DIR}")
set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION "${RTS_TEST_WINDOWS_KITS_VERSION}")

rts_collect_native_msvc_runtime_dlls()
if(NOT RTS_NATIVE_MSVC_RUNTIME_DLLS_RELEASE OR
        NOT RTS_NATIVE_MSVC_RUNTIME_DLLS_DEBUG)
    message(FATAL_ERROR "The fixture did not resolve both MSVC runtime sets.")
endif()

set(_fixture_generals_executable
    "${CMAKE_CURRENT_BINARY_DIR}/generalsv${RTS_BUILD_OUTPUT_SUFFIX}.exe")
set(_fixture_zero_hour_executable
    "${CMAKE_CURRENT_BINARY_DIR}/generalszh${RTS_BUILD_OUTPUT_SUFFIX}.exe")
file(WRITE "${_fixture_generals_executable}" "generals packaging fixture\n")
file(WRITE "${_fixture_zero_hour_executable}" "zero hour packaging fixture\n")

add_executable(g_generals IMPORTED GLOBAL)
set_property(TARGET g_generals PROPERTY IMPORTED_LOCATION "${_fixture_generals_executable}")
add_executable(z_generals IMPORTED GLOBAL)
set_property(TARGET z_generals PROPERTY IMPORTED_LOCATION "${_fixture_zero_hour_executable}")

set(_fixture_generals_launcher "${CMAKE_CURRENT_BINARY_DIR}/generals-launcher.lcf")
set(_fixture_zero_hour_launcher "${CMAKE_CURRENT_BINARY_DIR}/zero-hour-launcher.lcf")
rts_generate_launcher_lcf(g_generals "${_fixture_generals_launcher}")
rts_generate_launcher_lcf(z_generals "${_fixture_zero_hour_launcher}")

install(FILES "${_fixture_generals_executable}" DESTINATION Generals)
install(FILES "${_fixture_generals_launcher}" DESTINATION Generals RENAME launcher.lcf)
install(FILES "${_fixture_zero_hour_executable}" DESTINATION ZeroHour)
install(FILES "${_fixture_zero_hour_launcher}" DESTINATION ZeroHour RENAME launcher.lcf)
install(FILES ${RTS_NATIVE_MSVC_RUNTIME_DLLS_RELEASE}
    DESTINATION Generals
    CONFIGURATIONS Release RelWithDebInfo MinSizeRel)
install(FILES ${RTS_NATIVE_MSVC_RUNTIME_DLLS_RELEASE}
    DESTINATION ZeroHour
    CONFIGURATIONS Release RelWithDebInfo MinSizeRel)
install(FILES ${RTS_NATIVE_MSVC_RUNTIME_DLLS_DEBUG}
    DESTINATION Generals
    CONFIGURATIONS Debug)
install(FILES ${RTS_NATIVE_MSVC_RUNTIME_DLLS_DEBUG}
    DESTINATION ZeroHour
    CONFIGURATIONS Debug)
