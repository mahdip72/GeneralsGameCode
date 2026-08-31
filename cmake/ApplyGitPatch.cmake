if(NOT DEFINED PATCH_GIT_EXECUTABLE OR NOT DEFINED PATCH_SOURCE_DIR OR
    NOT DEFINED PATCH_FILE)
    message(FATAL_ERROR "ApplyGitPatch.cmake requires Git, source, and patch paths.")
endif()

execute_process(
    COMMAND "${PATCH_GIT_EXECUTABLE}" apply --check --reverse "${PATCH_FILE}"
    WORKING_DIRECTORY "${PATCH_SOURCE_DIR}"
    RESULT_VARIABLE _already_applied
    OUTPUT_QUIET
    ERROR_QUIET
)
if(_already_applied EQUAL 0)
    return()
endif()

if(DEFINED PATCH_SENTINEL_COUNT)
    if(NOT PATCH_SENTINEL_COUNT MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR
            "PATCH_SENTINEL_COUNT must be a positive integer when provided.")
    endif()

    set(_all_sentinels_found TRUE)
    foreach(_sentinel_index RANGE 1 ${PATCH_SENTINEL_COUNT})
        set(_file_variable "PATCH_SENTINEL_FILE_${_sentinel_index}")
        set(_text_variable "PATCH_SENTINEL_TEXT_${_sentinel_index}")
        if(NOT DEFINED ${_file_variable} OR NOT DEFINED ${_text_variable})
            message(FATAL_ERROR
                "Both ${_file_variable} and ${_text_variable} are required.")
        endif()

        set(_sentinel_path "${PATCH_SOURCE_DIR}/${${_file_variable}}")
        if(NOT EXISTS "${_sentinel_path}")
            set(_all_sentinels_found FALSE)
            continue()
        endif()

        file(READ "${_sentinel_path}" _sentinel_content)
        string(FIND "${_sentinel_content}" "${${_text_variable}}"
            _sentinel_position)
        if(_sentinel_position EQUAL -1)
            set(_all_sentinels_found FALSE)
        endif()
    endforeach()

    # A later patch may legitimately change context inside an earlier patch,
    # making that earlier patch's reverse check fail. Only accept that state
    # when every explicitly named hunk sentinel is present; one incidental
    # token must never make an incomplete patch look applied.
    if(_all_sentinels_found)
        return()
    endif()
endif()

execute_process(
    COMMAND "${PATCH_GIT_EXECUTABLE}" apply --check --whitespace=nowarn "${PATCH_FILE}"
    WORKING_DIRECTORY "${PATCH_SOURCE_DIR}"
    RESULT_VARIABLE _patch_check_result
    OUTPUT_VARIABLE _patch_output
    ERROR_VARIABLE _patch_error
)
if(NOT _patch_check_result EQUAL 0)
    message(FATAL_ERROR
        "Patch is neither fully applied nor cleanly applicable: ${PATCH_FILE}\n"
        "${_patch_output}\n${_patch_error}")
endif()

execute_process(
    COMMAND "${PATCH_GIT_EXECUTABLE}" apply --whitespace=nowarn "${PATCH_FILE}"
    WORKING_DIRECTORY "${PATCH_SOURCE_DIR}"
    RESULT_VARIABLE _patch_result
    OUTPUT_VARIABLE _patch_output
    ERROR_VARIABLE _patch_error
)
if(NOT _patch_result EQUAL 0)
    message(FATAL_ERROR
        "Failed to apply ${PATCH_FILE} after a successful applicability check:\n"
        "${_patch_output}\n${_patch_error}")
endif()
