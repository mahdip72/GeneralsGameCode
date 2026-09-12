# Shared packaging helpers for native x64 product installs.

function(rts_collect_native_msvc_runtime_dlls)
    if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(RTS_NATIVE_MSVC_RUNTIME_DLLS_RELEASE "" PARENT_SCOPE)
        set(RTS_NATIVE_MSVC_RUNTIME_DLLS_DEBUG "" PARENT_SCOPE)
        return()
    endif()

    # InstallRequiredSystemLibraries has process-wide-looking variables and may
    # be included more than once.  Keep each invocation in this function's
    # scope, and collect the release and debug sets independently.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    set(CMAKE_INSTALL_UCRT_LIBRARIES TRUE)

    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS "")
    set(CMAKE_INSTALL_DEBUG_LIBRARIES FALSE)
    set(CMAKE_INSTALL_DEBUG_LIBRARIES_ONLY FALSE)
    include(InstallRequiredSystemLibraries)
    set(_rts_release_dlls ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS})

    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS "")
    set(CMAKE_INSTALL_DEBUG_LIBRARIES TRUE)
    set(CMAKE_INSTALL_DEBUG_LIBRARIES_ONLY TRUE)
    include(InstallRequiredSystemLibraries)
    set(_rts_debug_dlls ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS})

    list(REMOVE_DUPLICATES _rts_release_dlls)
    list(REMOVE_DUPLICATES _rts_debug_dlls)
    foreach(_rts_runtime_dll IN LISTS _rts_release_dlls _rts_debug_dlls)
        if(NOT EXISTS "${_rts_runtime_dll}")
            message(FATAL_ERROR
                "Resolved native x64 MSVC runtime does not exist: ${_rts_runtime_dll}")
        endif()
    endforeach()

    set(RTS_NATIVE_MSVC_RUNTIME_DLLS_RELEASE "${_rts_release_dlls}" PARENT_SCOPE)
    set(RTS_NATIVE_MSVC_RUNTIME_DLLS_DEBUG "${_rts_debug_dlls}" PARENT_SCOPE)
endfunction()

function(rts_generate_launcher_lcf target output_path)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Cannot generate a launcher for unknown target '${target}'.")
    endif()

    set(_rts_launcher_arguments "")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        # Native product launches select the final Stage 5 policy. The launcher
        # appends its own command-line arguments after these defaults, so
        # `launcher.exe -simulationMode serial` remains an explicit fallback.
        set(_rts_launcher_arguments
            " -simulationMode parallel -workerPolicy auto")
    endif()
    file(GENERATE
        OUTPUT "${output_path}"
        CONTENT "RUN = . $<TARGET_FILE_NAME:${target}>${_rts_launcher_arguments}\n")
endfunction()
