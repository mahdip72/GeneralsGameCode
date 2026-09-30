include_guard(GLOBAL)

function(_rts_classify_target_architecture out_architecture value kind)
    string(STRIP "${value}" _value)
    string(TOLOWER "${_value}" _value)

    if(kind STREQUAL "platform")
        # Visual Studio may suffix the platform with a Windows SDK version.
        string(REGEX REPLACE ",.*$" "" _value "${_value}")
    endif()

    if(kind STREQUAL "triple")
        if(_value MATCHES "^(x64|amd64|x86_64)(-|$)")
            set(_architecture AMD64)
        elseif(_value MATCHES "^(arm64|arm64ec|aarch64|arm|x86|i[3-6]86|i[0-9]86|ia64)(-|$)")
            set(_architecture NON_AMD64)
        else()
            set(_architecture UNKNOWN)
        endif()
    elseif(_value MATCHES "^(x64|amd64|x86_64)$")
        set(_architecture AMD64)
    elseif(_value MATCHES "^(arm64|arm64ec|aarch64|arm|x86|win32|i[3-6]86|i[0-9]86|ia64)$")
        set(_architecture NON_AMD64)
    else()
        set(_architecture UNKNOWN)
    endif()

    set(${out_architecture} "${_architecture}" PARENT_SCOPE)
endfunction()

function(rts_native_target_architecture_is_amd64 out_is_amd64 out_reason)
    set(_target_signals)

    # CMAKE_GENERATOR_PLATFORM is target metadata only for generators that
    # actually consume a platform selection. In particular, do not accept a
    # stray value as a Ninja target override.
    if((CMAKE_GENERATOR MATCHES "^Visual Studio" OR
            CMAKE_GENERATOR MATCHES "^Green Hills MULTI") AND
            DEFINED CMAKE_GENERATOR_PLATFORM AND
            NOT "${CMAKE_GENERATOR_PLATFORM}" STREQUAL "")
        list(APPEND _target_signals CMAKE_GENERATOR_PLATFORM)
    endif()
    if(CMAKE_GENERATOR MATCHES "^Visual Studio" AND
            DEFINED CMAKE_VS_PLATFORM_NAME AND
            NOT "${CMAKE_VS_PLATFORM_NAME}" STREQUAL "")
        list(APPEND _target_signals CMAKE_VS_PLATFORM_NAME)
    endif()

    foreach(_language C CXX)
        foreach(_suffix COMPILER_ARCHITECTURE_ID COMPILER_TARGET)
            set(_signal "CMAKE_${_language}_${_suffix}")
            if(DEFINED ${_signal} AND NOT "${${_signal}}" STREQUAL "")
                list(APPEND _target_signals "${_signal}")
            endif()
        endforeach()
    endforeach()

    set(_observed_signals "")
    set(_separator "")
    set(_all_authoritative_signals_amd64 TRUE)
    foreach(_signal IN LISTS _target_signals)
        set(_value "${${_signal}}")
        if(_signal MATCHES "_COMPILER_TARGET$")
            _rts_classify_target_architecture(_architecture "${_value}" triple)
        elseif(_signal MATCHES "GENERATOR_PLATFORM$")
            _rts_classify_target_architecture(_architecture "${_value}" platform)
        else()
            _rts_classify_target_architecture(_architecture "${_value}" identity)
        endif()

        string(APPEND _observed_signals
            "${_separator}${_signal}='${_value}' -> ${_architecture}")
        set(_separator "; ")
        if(NOT _architecture STREQUAL "AMD64")
            set(_all_authoritative_signals_amd64 FALSE)
        endif()
    endforeach()

    if(_target_signals)
        if(_all_authoritative_signals_amd64)
            set(_is_amd64 TRUE)
            set(_reason "authoritative target signals agree: ${_observed_signals}")
        else()
            set(_is_amd64 FALSE)
            set(_reason
                "authoritative target signals must all identify AMD64/x64: ${_observed_signals}")
        endif()
    # CMAKE_SYSTEM_PROCESSOR may mirror the host for native builds. Use it only
    # when CMake exposes no more authoritative platform/compiler target signal.
    elseif(DEFINED CMAKE_SYSTEM_PROCESSOR AND
            NOT "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "")
        _rts_classify_target_architecture(
            _architecture "${CMAKE_SYSTEM_PROCESSOR}" processor)
        if(_architecture STREQUAL "AMD64")
            set(_is_amd64 TRUE)
            set(_reason
                "no authoritative target signal was available; CMAKE_SYSTEM_PROCESSOR='${CMAKE_SYSTEM_PROCESSOR}' identifies AMD64/x64")
        else()
            set(_is_amd64 FALSE)
            set(_reason
                "no authoritative target signal was available and CMAKE_SYSTEM_PROCESSOR='${CMAKE_SYSTEM_PROCESSOR}' resolves to ${_architecture}")
        endif()
    else()
        set(_is_amd64 FALSE)
        set(_reason
            "no authoritative target signal or CMAKE_SYSTEM_PROCESSOR was available")
    endif()

    set(${out_is_amd64} "${_is_amd64}" PARENT_SCOPE)
    set(${out_reason} "${_reason}" PARENT_SCOPE)
endfunction()

function(rts_require_native_amd64_target context)
    rts_native_target_architecture_is_amd64(_is_amd64 _reason)
    if(NOT _is_amd64)
        message(FATAL_ERROR
            "${context} requires an AMD64/x64 target architecture: ${_reason}.")
    endif()
endfunction()
