include_guard(GLOBAL)

function(rts_find_windows_sdk_program output_variable)
    cmake_parse_arguments(PARSE_ARGV 1 argument "" "" "NAMES")
    if(NOT argument_NAMES)
        message(FATAL_ERROR "rts_find_windows_sdk_program requires NAMES.")
    endif()

    set(windows_sdk_roots)
    get_filename_component(windows_kits_registry_root
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]"
        ABSOLUTE
    )
    if(windows_kits_registry_root)
        list(APPEND windows_sdk_roots "${windows_kits_registry_root}")
    endif()
    if(DEFINED ENV{WindowsSdkDir} AND NOT "$ENV{WindowsSdkDir}" STREQUAL "")
        list(APPEND windows_sdk_roots "$ENV{WindowsSdkDir}")
    endif()
    list(REMOVE_DUPLICATES windows_sdk_roots)

    set(windows_sdk_tool_hints)
    foreach(windows_sdk_root IN LISTS windows_sdk_roots)
        set(windows_sdk_bin_dirs)
        if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
            list(APPEND windows_sdk_bin_dirs
                "${windows_sdk_root}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
        endif()
        if(DEFINED ENV{WindowsSDKVersion} AND NOT "$ENV{WindowsSDKVersion}" STREQUAL "")
            set(windows_sdk_environment_version "$ENV{WindowsSDKVersion}")
            string(REGEX REPLACE "[\\\\/]+$" "" windows_sdk_environment_version
                "${windows_sdk_environment_version}")
            list(APPEND windows_sdk_bin_dirs
                "${windows_sdk_root}/bin/${windows_sdk_environment_version}")
        endif()

        file(GLOB windows_sdk_discovered_bin_dirs LIST_DIRECTORIES true
            "${windows_sdk_root}/bin/10.*")
        list(SORT windows_sdk_discovered_bin_dirs COMPARE NATURAL ORDER DESCENDING)
        list(APPEND windows_sdk_bin_dirs ${windows_sdk_discovered_bin_dirs})
        list(REMOVE_DUPLICATES windows_sdk_bin_dirs)

        foreach(windows_sdk_bin_dir IN LISTS windows_sdk_bin_dirs)
            # The compiler is a host tool. Prefer the 64-bit host binary but
            # retain the x86 variant for 32-bit Windows hosts and older SDKs.
            list(APPEND windows_sdk_tool_hints
                "${windows_sdk_bin_dir}/x64"
                "${windows_sdk_bin_dir}/x86")
        endforeach()
    endforeach()

    find_program(${output_variable}
        NAMES ${argument_NAMES}
        HINTS ${windows_sdk_tool_hints}
    )
    if(NOT ${output_variable})
        list(JOIN argument_NAMES ", " requested_program_names)
        message(FATAL_ERROR
            "Required Windows SDK program was not found (${requested_program_names}).")
    endif()
    set(${output_variable} "${${output_variable}}" PARENT_SCOPE)
endfunction()
