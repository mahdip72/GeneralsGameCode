# CMake loads this C++ rules override after its platform module has populated
# CMAKE_CXX_FLAGS_INIT, but before that value initializes the CMAKE_CXX_FLAGS
# cache entry. CMake's Windows-MSVC defaults add /Zm1000 to old MSVC C++ flags;
# the VC6 x86 authoring lane has a controlled C1060 failure with that default.
# Remove only the platform-owned /Zm1000 token and retain the rest of the
# platform flag block (notably /GX) and any user-provided prefix/suffix.

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR
   NOT DEFINED MSVC_VERSION OR MSVC_VERSION GREATER_EQUAL 1300)
    return()
endif()

if(NOT DEFINED _FLAGS_CXX)
    return()
endif()

separate_arguments(_rts_vc6_platform_tokens WINDOWS_COMMAND "${_FLAGS_CXX}")
set(_rts_vc6_zm_count 0)
foreach(_rts_vc6_platform_token IN LISTS _rts_vc6_platform_tokens)
    if(_rts_vc6_platform_token STREQUAL "/Zm1000")
        math(EXPR _rts_vc6_zm_count "${_rts_vc6_zm_count} + 1")
    endif()
endforeach()
if(_rts_vc6_zm_count EQUAL 0)
    # A CMake version which no longer supplies this default needs no override.
    return()
elseif(NOT _rts_vc6_zm_count EQUAL 1)
    message(FATAL_ERROR "Unexpected VC6 platform C++ /Zm1000 flag count: ${_rts_vc6_zm_count}")
endif()

# Locate CMake's own platform flag block in the initialization string. Refuse
# an ambiguous or missing match rather than rewriting user-supplied flags.
string(FIND "${CMAKE_CXX_FLAGS_INIT}" "${_FLAGS_CXX}" _rts_vc6_flags_offset)
if(_rts_vc6_flags_offset LESS 0)
    message(FATAL_ERROR "VC6 platform C++ flags were not present in CMAKE_CXX_FLAGS_INIT")
endif()

string(LENGTH "${_FLAGS_CXX}" _rts_vc6_flags_length)
math(EXPR _rts_vc6_after_flags_offset "${_rts_vc6_flags_offset} + ${_rts_vc6_flags_length}")
string(SUBSTRING "${CMAKE_CXX_FLAGS_INIT}" ${_rts_vc6_after_flags_offset} -1 _rts_vc6_flags_after)
string(FIND "${_rts_vc6_flags_after}" "${_FLAGS_CXX}" _rts_vc6_duplicate_offset)
if(NOT _rts_vc6_duplicate_offset LESS 0)
    message(FATAL_ERROR "VC6 platform C++ flag block is ambiguous in CMAKE_CXX_FLAGS_INIT")
endif()

# Remove the one token from CMake's platform block without changing /GX or any
# explicit CXXFLAGS/CMAKE_CXX_FLAGS override. Consume the separator after the
# token when present, otherwise consume the separator before it. This avoids
# introducing duplicate spacing while preserving the block's outer boundary.
# CMAKE_CXX_FLAGS is intentionally not FORCE-updated; existing build caches
# retain their prior initialization.
string(LENGTH "${_FLAGS_CXX}" _rts_vc6_platform_length)
string(LENGTH "/Zm1000" _rts_vc6_zm_length)
set(_rts_vc6_search_offset 0)
set(_rts_vc6_zm_offset -1)
while(_rts_vc6_search_offset LESS _rts_vc6_platform_length)
    string(SUBSTRING "${_FLAGS_CXX}" ${_rts_vc6_search_offset} -1 _rts_vc6_remaining_flags)
    string(FIND "${_rts_vc6_remaining_flags}" "/Zm1000" _rts_vc6_relative_offset)
    if(_rts_vc6_relative_offset LESS 0)
        break()
    endif()
    math(EXPR _rts_vc6_candidate_offset "${_rts_vc6_search_offset} + ${_rts_vc6_relative_offset}")
    math(EXPR _rts_vc6_after_candidate "${_rts_vc6_candidate_offset} + ${_rts_vc6_zm_length}")

    set(_rts_vc6_token_start TRUE)
    if(_rts_vc6_candidate_offset GREATER 0)
        math(EXPR _rts_vc6_before_candidate "${_rts_vc6_candidate_offset} - 1")
        string(SUBSTRING "${_FLAGS_CXX}" ${_rts_vc6_before_candidate} 1 _rts_vc6_before_character)
        if(NOT _rts_vc6_before_character MATCHES "[ \t\r\n]")
            set(_rts_vc6_token_start FALSE)
        endif()
    endif()

    set(_rts_vc6_token_end TRUE)
    if(_rts_vc6_after_candidate LESS _rts_vc6_platform_length)
        string(SUBSTRING "${_FLAGS_CXX}" ${_rts_vc6_after_candidate} 1 _rts_vc6_after_character)
        if(NOT _rts_vc6_after_character MATCHES "[ \t\r\n]")
            set(_rts_vc6_token_end FALSE)
        endif()
    endif()

    if(_rts_vc6_token_start AND _rts_vc6_token_end)
        if(NOT _rts_vc6_zm_offset LESS 0)
            message(FATAL_ERROR "VC6 platform C++ /Zm1000 exact token location is ambiguous")
        endif()
        set(_rts_vc6_zm_offset ${_rts_vc6_candidate_offset})
    endif()
    set(_rts_vc6_search_offset ${_rts_vc6_after_candidate})
endwhile()
if(_rts_vc6_zm_offset LESS 0)
    message(FATAL_ERROR "VC6 platform C++ /Zm1000 count/location disagreement")
endif()
math(EXPR _rts_vc6_suffix_offset "${_rts_vc6_zm_offset} + ${_rts_vc6_zm_length}")
string(SUBSTRING "${_FLAGS_CXX}" 0 ${_rts_vc6_zm_offset} _rts_vc6_flag_prefix)
string(SUBSTRING "${_FLAGS_CXX}" ${_rts_vc6_suffix_offset} -1 _rts_vc6_flag_suffix)
if(_rts_vc6_flag_suffix MATCHES "^[ \t\r\n]+")
    string(REGEX REPLACE "^[ \t\r\n]+" "" _rts_vc6_flag_suffix "${_rts_vc6_flag_suffix}")
elseif(_rts_vc6_flag_prefix MATCHES "[ \t\r\n]+$")
    string(REGEX REPLACE "[ \t\r\n]+$" "" _rts_vc6_flag_prefix "${_rts_vc6_flag_prefix}")
endif()
set(_rts_vc6_flags_without_zm "${_rts_vc6_flag_prefix}${_rts_vc6_flag_suffix}")

string(SUBSTRING "${CMAKE_CXX_FLAGS_INIT}" 0 ${_rts_vc6_flags_offset} _rts_vc6_init_prefix)
set(CMAKE_CXX_FLAGS_INIT
    "${_rts_vc6_init_prefix}${_rts_vc6_flags_without_zm}${_rts_vc6_flags_after}")
message(STATUS "VC6: removed CMake's default /Zm1000 C++ reservation; retained remaining compiler flags")
