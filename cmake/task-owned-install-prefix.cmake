# Validation helpers for disposable VC6 product install destinations.
#
# VC6 install destinations are deliberately separate from registry discovery.
# The caller must provide an install root that is either the build's dedicated
# install subtree or a child of an explicitly approved scratch root.  Resolve
# both paths before comparing them so an existing junction/symlink cannot turn
# an apparently safe destination into a canonical or shared runtime.

function(_rts_path_is_under child parent output_variable)
    file(TO_CMAKE_PATH "${child}" _rts_child_normalized)
    file(TO_CMAKE_PATH "${parent}" _rts_parent_normalized)
    string(REGEX REPLACE "/+$" "" _rts_child_normalized "${_rts_child_normalized}")
    string(REGEX REPLACE "/+$" "" _rts_parent_normalized "${_rts_parent_normalized}")
    string(TOLOWER "${_rts_child_normalized}" _rts_child_lower)
    string(TOLOWER "${_rts_parent_normalized}" _rts_parent_lower)

    set(_rts_is_under FALSE)
    if(NOT "${_rts_child_lower}" STREQUAL "${_rts_parent_lower}")
        string(LENGTH "${_rts_parent_lower}" _rts_parent_length)
        string(SUBSTRING "${_rts_child_lower}" 0 ${_rts_parent_length}
            _rts_child_parent_prefix)
        if("${_rts_child_parent_prefix}" STREQUAL "${_rts_parent_lower}")
            string(SUBSTRING "${_rts_child_lower}" ${_rts_parent_length} -1
                _rts_child_suffix)
            if("${_rts_child_suffix}" MATCHES "^/")
                set(_rts_is_under TRUE)
            endif()
        endif()
    endif()
    set(${output_variable} ${_rts_is_under} PARENT_SCOPE)
endfunction()

function(_rts_path_is_equal left right output_variable)
    file(TO_CMAKE_PATH "${left}" _rts_left_normalized)
    file(TO_CMAKE_PATH "${right}" _rts_right_normalized)
    string(REGEX REPLACE "/+$" "" _rts_left_normalized "${_rts_left_normalized}")
    string(REGEX REPLACE "/+$" "" _rts_right_normalized "${_rts_right_normalized}")
    string(TOLOWER "${_rts_left_normalized}" _rts_left_lower)
    string(TOLOWER "${_rts_right_normalized}" _rts_right_lower)
    if("${_rts_left_lower}" STREQUAL "${_rts_right_lower}")
        set(${output_variable} TRUE PARENT_SCOPE)
    else()
        set(${output_variable} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(_rts_reject_windows_symlink_ancestors path context)
    if(NOT CMAKE_HOST_WIN32)
        return()
    endif()

    # CMake versions used by the compatibility lane can report a lexical
    # REAL_PATH for a junction.  Inspect every existing component first so a
    # missing leaf cannot hide a reparse point in its parent chain.
    get_filename_component(_rts_probe_path "${path}" ABSOLUTE)
    file(TO_CMAKE_PATH "${_rts_probe_path}" _rts_probe_path)
    while(TRUE)
        if(IS_SYMLINK "${_rts_probe_path}")
            message(FATAL_ERROR
                "${context} must resolve below RTS_TASK_OWNED_INSTALL_ROOT; "
                "junctions, traversal, and absolute canonical destinations are rejected. "
                "Existing path component '${_rts_probe_path}' is a junction or symlink.")
        endif()
        get_filename_component(_rts_probe_parent "${_rts_probe_path}" DIRECTORY)
        if("${_rts_probe_parent}" STREQUAL "${_rts_probe_path}")
            break()
        endif()
        set(_rts_probe_path "${_rts_probe_parent}")
    endwhile()
endfunction()

function(_rts_resolve_path_with_existing_parent path output_variable context)
    # REAL_PATH intentionally requires an existing path.  A normal configure
    # has not created the install leaf yet, so walk to the nearest existing
    # directory, resolve that directory, and append the lexical remainder.
    # This still resolves every existing junction/symlink in the path.
    get_filename_component(_rts_absolute_path "${path}" ABSOLUTE)
    _rts_reject_windows_symlink_ancestors("${_rts_absolute_path}" "${context}")
    file(TO_CMAKE_PATH "${_rts_absolute_path}" _rts_candidate)
    set(_rts_suffix "")
    while(NOT EXISTS "${_rts_candidate}")
        get_filename_component(_rts_leaf "${_rts_candidate}" NAME)
        if("${_rts_leaf}" STREQUAL "")
            message(FATAL_ERROR
                "${context} cannot resolve an existing parent directory for '${path}'.")
        endif()
        if("${_rts_suffix}" STREQUAL "")
            set(_rts_suffix "${_rts_leaf}")
        else()
            set(_rts_suffix "${_rts_leaf}/${_rts_suffix}")
        endif()
        get_filename_component(_rts_parent "${_rts_candidate}" DIRECTORY)
        if("${_rts_parent}" STREQUAL "${_rts_candidate}")
            message(FATAL_ERROR
                "${context} cannot resolve an existing parent directory for '${path}'.")
        endif()
        set(_rts_candidate "${_rts_parent}")
    endwhile()
    if(NOT IS_DIRECTORY "${_rts_candidate}")
        message(FATAL_ERROR
            "${context} has a file where a directory is required: '${_rts_candidate}'.")
    endif()
    file(REAL_PATH "${_rts_candidate}" _rts_existing_real)
    if("${_rts_suffix}" STREQUAL "")
        set(_rts_resolved "${_rts_existing_real}")
    else()
        set(_rts_resolved "${_rts_existing_real}/${_rts_suffix}")
    endif()
    file(TO_CMAKE_PATH "${_rts_resolved}" _rts_resolved_normalized)
    set(${output_variable} "${_rts_resolved_normalized}" PARENT_SCOPE)
endfunction()

function(_rts_reject_shared_or_canonical_path path context)
    file(TO_CMAKE_PATH "${path}" _rts_path_normalized)
    string(TOLOWER "${_rts_path_normalized}" _rts_path_lower)
    if("${_rts_path_lower}" MATCHES "^//" OR
            "${_rts_path_lower}" MATCHES "(^|/)(users|documents and settings|public|programdata|windows|program files|program files \\(x86\\)|steamapps|steamlibrary)(/|$)" OR
            "${_rts_path_lower}" MATCHES "(^|/)(generalsgamecode-(generals|zerohour)|command and conquer generals( zero hour)?|ggc-stage[0-9][^/]*)($|/)")
        message(FATAL_ERROR
            "${context} resolves to a user, canonical, shared, or retail path; "
            "use a task-owned disposable install root instead: '${path}'.")
    endif()
endfunction()

function(rts_validate_task_owned_vc6_install_prefix prefix variable_name)
    if("${prefix}" STREQUAL "")
        message(FATAL_ERROR
            "${variable_name} is required before installing a VC6 product; "
            "provide an absolute task-owned disposable prefix.")
    endif()
    if(NOT IS_ABSOLUTE "${prefix}")
        message(FATAL_ERROR
            "${variable_name} must be an absolute task-owned disposable prefix, got '${prefix}'.")
    endif()
    if(NOT DEFINED RTS_TASK_OWNED_INSTALL_ROOT OR
            "${RTS_TASK_OWNED_INSTALL_ROOT}" STREQUAL "")
        message(FATAL_ERROR
            "RTS_TASK_OWNED_INSTALL_ROOT is required before installing a VC6 product; "
            "registry and implicit user paths are not valid install roots.")
    endif()
    if(NOT IS_ABSOLUTE "${RTS_TASK_OWNED_INSTALL_ROOT}")
        message(FATAL_ERROR
            "RTS_TASK_OWNED_INSTALL_ROOT must be an absolute disposable root, got "
            "'${RTS_TASK_OWNED_INSTALL_ROOT}'.")
    endif()

    _rts_resolve_path_with_existing_parent("${RTS_TASK_OWNED_INSTALL_ROOT}"
        _rts_install_root_real "RTS_TASK_OWNED_INSTALL_ROOT")
    _rts_resolve_path_with_existing_parent("${prefix}" _rts_prefix_real
        "${variable_name}")
    _rts_resolve_path_with_existing_parent("${CMAKE_BINARY_DIR}/install"
        _rts_build_install_root_real "CMake build install root")
    _rts_reject_shared_or_canonical_path("${_rts_install_root_real}"
        "RTS_TASK_OWNED_INSTALL_ROOT")
    _rts_reject_shared_or_canonical_path("${_rts_prefix_real}" "${variable_name}")

    _rts_path_is_under("${_rts_install_root_real}" "${_rts_build_install_root_real}"
        _rts_root_under_build_install)
    _rts_path_is_equal("${_rts_install_root_real}" "${_rts_build_install_root_real}"
        _rts_root_is_build_install)
    if(_rts_root_is_build_install)
        set(_rts_root_under_build_install TRUE)
    endif()
    set(_rts_root_is_approved FALSE)
    if(DEFINED RTS_APPROVED_SCRATCH_ROOT AND
            NOT "${RTS_APPROVED_SCRATCH_ROOT}" STREQUAL "")
        if(NOT IS_ABSOLUTE "${RTS_APPROVED_SCRATCH_ROOT}")
            message(FATAL_ERROR
                "RTS_APPROVED_SCRATCH_ROOT must be an absolute approved scratch root, got "
                "'${RTS_APPROVED_SCRATCH_ROOT}'.")
        endif()
        _rts_resolve_path_with_existing_parent("${RTS_APPROVED_SCRATCH_ROOT}"
            _rts_approved_root_real "RTS_APPROVED_SCRATCH_ROOT")
        _rts_reject_shared_or_canonical_path("${_rts_approved_root_real}"
            "RTS_APPROVED_SCRATCH_ROOT")
        _rts_path_is_under("${_rts_install_root_real}" "${_rts_approved_root_real}"
            _rts_root_under_approved_scratch)
        _rts_path_is_equal("${_rts_install_root_real}" "${_rts_approved_root_real}"
            _rts_root_is_approved_scratch)
        if(_rts_root_under_approved_scratch OR _rts_root_is_approved_scratch)
            set(_rts_root_is_approved TRUE)
        endif()
    endif()

    if(NOT _rts_root_under_build_install AND NOT _rts_root_is_approved)
        message(FATAL_ERROR
            "RTS_TASK_OWNED_INSTALL_ROOT must resolve below the build install root "
            "'${_rts_build_install_root_real}' or an explicit RTS_APPROVED_SCRATCH_ROOT; "
            "got '${_rts_install_root_real}'.")
    endif()

    _rts_path_is_under("${_rts_prefix_real}" "${_rts_install_root_real}"
        _rts_prefix_under_root)
    if(NOT _rts_prefix_under_root)
        message(FATAL_ERROR
            "${variable_name} must resolve below RTS_TASK_OWNED_INSTALL_ROOT; "
            "junctions, traversal, and absolute canonical destinations are rejected. "
            "Root='${_rts_install_root_real}', prefix='${_rts_prefix_real}'.")
    endif()
    if("${_rts_prefix_real}" STREQUAL "${_rts_install_root_real}")
        message(FATAL_ERROR
            "${variable_name} must name a title-specific child below "
            "RTS_TASK_OWNED_INSTALL_ROOT, not the shared root itself.")
    endif()
endfunction()
