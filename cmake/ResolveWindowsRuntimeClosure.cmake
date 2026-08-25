cmake_minimum_required(VERSION 3.25)

foreach(_required_variable IN ITEMS ROOT_DLLS RUNTIME_DIR OUTPUT_FILE)
    if(NOT DEFINED ${_required_variable} OR "${${_required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${_required_variable} is required.")
    endif()
endforeach()

file(REAL_PATH "${RUNTIME_DIR}" _runtime_directory)
if(NOT IS_DIRECTORY "${_runtime_directory}")
    message(FATAL_ERROR "Runtime directory does not exist: ${_runtime_directory}")
endif()
string(REPLACE "\\" "/" _runtime_directory "${_runtime_directory}")
string(REGEX REPLACE "([][+.*()^$?|])" "\\\\\\1"
    _runtime_directory_regex "${_runtime_directory}")
string(REPLACE "/" "[/\\\\]" _runtime_directory_regex
    "${_runtime_directory_regex}")

set(_runtime_closure)
foreach(_root_dll IN LISTS ROOT_DLLS)
    file(REAL_PATH "${_root_dll}" _root_dll_path)
    if(NOT EXISTS "${_root_dll_path}")
        message(FATAL_ERROR "Runtime root DLL does not exist: ${_root_dll_path}")
    endif()
    cmake_path(IS_PREFIX _runtime_directory "${_root_dll_path}"
        NORMALIZE _root_is_app_local)
    if(NOT _root_is_app_local)
        message(FATAL_ERROR
            "Runtime root DLL is outside the declared runtime directory: ${_root_dll_path}")
    endif()
    string(REPLACE "\\" "/" _root_dll_path "${_root_dll_path}")
    list(APPEND _runtime_closure "${_root_dll_path}")
endforeach()

if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()
file(GET_RUNTIME_DEPENDENCIES
    LIBRARIES ${_runtime_closure}
    DIRECTORIES "${_runtime_directory}"
    RESOLVED_DEPENDENCIES_VAR _resolved_dependencies
    UNRESOLVED_DEPENDENCIES_VAR _unresolved_dependencies
    CONFLICTING_DEPENDENCIES_PREFIX _conflicts
    PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
    POST_INCLUDE_REGEXES "^${_runtime_directory_regex}/"
    POST_EXCLUDE_REGEXES "^[A-Za-z]:[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\]")

if(_conflicts_FILENAMES)
    message(FATAL_ERROR
        "Runtime dependency closure contains conflicting DLL names: ${_conflicts_FILENAMES}")
endif()
if(_unresolved_dependencies)
    message(FATAL_ERROR
        "Runtime dependency closure is incomplete in ${_runtime_directory}: ${_unresolved_dependencies}")
endif()

foreach(_dependency IN LISTS _resolved_dependencies)
    file(REAL_PATH "${_dependency}" _dependency_path)
    cmake_path(IS_PREFIX _runtime_directory "${_dependency_path}"
        NORMALIZE _dependency_is_app_local)
    if(_dependency_is_app_local)
        string(REPLACE "\\" "/" _dependency_path "${_dependency_path}")
        list(APPEND _runtime_closure "${_dependency_path}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _runtime_closure)
list(SORT _runtime_closure)
file(WRITE "${OUTPUT_FILE}" "${_runtime_closure}")
