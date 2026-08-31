# Product title graphs call this helper in every configuration.  Define the
# no-op boundary before the ASan-only configure path returns so Release and
# Profile configuration cannot fail with an unknown CMake command.
function(rts_install_asan_runtime destination)
    if(RTS_BUILD_OPTION_ASAN AND RTS_NATIVE_ASAN_RUNTIME_DLLS)
        install(FILES ${RTS_NATIVE_ASAN_RUNTIME_DLLS}
            DESTINATION "${destination}")
    endif()
endfunction()

if(NOT RTS_BUILD_OPTION_ASAN OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    return()
endif()

# MSVC's /fsanitize=address link step records the dynamic runtime by name, but
# it does not copy that DLL beside build-tree executables.  Resolve it from the
# active compiler directory so the build never depends on a machine-wide PATH.
get_filename_component(_rts_msvc_compiler_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
find_file(_rts_native_asan_runtime_dll
    NAMES clang_rt.asan_dynamic-x86_64.dll
    PATHS "${_rts_msvc_compiler_bin_dir}"
    NO_DEFAULT_PATH
    NO_CACHE)

if(NOT _rts_native_asan_runtime_dll)
    message(FATAL_ERROR
        "AddressSanitizer is enabled, but clang_rt.asan_dynamic-x86_64.dll "
        "was not found beside the active MSVC compiler '${CMAKE_CXX_COMPILER}'.")
endif()

set(RTS_NATIVE_ASAN_RUNTIME_DLLS "${_rts_native_asan_runtime_dll}"
    CACHE INTERNAL "Resolved app-local MSVC AddressSanitizer runtime DLLs" FORCE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_rts_native_asan_runtime_dll}")

# All source-owned executables in a product subtree share its configured
# runtime output directory. Stage the DLL during configure so direct launches
# and CTest never depend on a Visual Studio developer-shell PATH.
function(rts_stage_asan_runtime output_directory)
    if(CMAKE_CONFIGURATION_TYPES)
        foreach(_configuration IN LISTS CMAKE_CONFIGURATION_TYPES)
            set(_runtime_directory "${output_directory}/${_configuration}")
            file(MAKE_DIRECTORY "${_runtime_directory}")
            file(COPY_FILE "${RTS_NATIVE_ASAN_RUNTIME_DLLS}"
                "${_runtime_directory}/clang_rt.asan_dynamic-x86_64.dll"
                ONLY_IF_DIFFERENT)
        endforeach()
    else()
        file(MAKE_DIRECTORY "${output_directory}")
        file(COPY_FILE "${RTS_NATIVE_ASAN_RUNTIME_DLLS}"
            "${output_directory}/clang_rt.asan_dynamic-x86_64.dll"
            ONLY_IF_DIFFERENT)
    endif()
endfunction()
