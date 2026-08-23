# Native XAudio2 platform boundary.  The legacy Win32 product remains on
# Miles, so this target is intentionally absent from every 32-bit graph.
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    return()
endif()

add_library(rts_xaudio2 INTERFACE)

get_filename_component(_rts_windows_kits_dir
    "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]"
    ABSOLUTE
)
if(NOT _rts_windows_kits_dir)
    set(_rts_windows_kits_dir "$ENV{WindowsSdkDir}")
endif()

set(_rts_windows_sdk_version "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
if(NOT _rts_windows_sdk_version)
    set(_rts_windows_sdk_version "$ENV{WindowsSDKVersion}")
    string(REGEX REPLACE "[\\\\/]+$" "" _rts_windows_sdk_version "${_rts_windows_sdk_version}")
endif()

if(NOT _rts_windows_kits_dir OR NOT _rts_windows_sdk_version)
    message(FATAL_ERROR
        "Native XAudio2 requires a Windows 10 SDK with a resolved root and version.")
endif()

find_path(RTS_XAUDIO2_INCLUDE_DIR xaudio2.h
    HINTS "${_rts_windows_kits_dir}/Include/${_rts_windows_sdk_version}/um"
    NO_DEFAULT_PATH
)
find_library(RTS_XAUDIO2_LIBRARY xaudio2.lib
    HINTS "${_rts_windows_kits_dir}/Lib/${_rts_windows_sdk_version}/um/x64"
    NO_DEFAULT_PATH
)

if(NOT RTS_XAUDIO2_INCLUDE_DIR OR NOT RTS_XAUDIO2_LIBRARY)
    message(FATAL_ERROR
        "Native XAudio2 requires xaudio2.h and the x64 xaudio2.lib from the selected Windows 10 SDK.")
endif()

target_include_directories(rts_xaudio2 INTERFACE "${RTS_XAUDIO2_INCLUDE_DIR}")
target_link_libraries(rts_xaudio2 INTERFACE "${RTS_XAUDIO2_LIBRARY}")
target_compile_definitions(rts_xaudio2 INTERFACE
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    NTDDI_VERSION=NTDDI_WIN10_19H1
)
