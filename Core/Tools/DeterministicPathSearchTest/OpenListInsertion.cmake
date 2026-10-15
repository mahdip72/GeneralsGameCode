# Exercise live fixed-mode intrusive open-list methods without constructing a game.
# Standalone extraction: cmake -DOPEN_LIST_SOURCE_ROOT=... -DOPEN_LIST_BINARY_DIR=... -P ...
file(READ "${OPEN_LIST_SOURCE_ROOT}/Core/GameEngine/Source/GameLogic/AI/AIPathfind.cpp" _open_source)
file(READ "${OPEN_LIST_SOURCE_ROOT}/Core/GameEngine/Include/GameLogic/AIPathfind.h" _open_header)
string(REPLACE "\r\n" "\n" _open_source "${_open_source}")
string(REPLACE "\r\n" "\n" _open_header "${_open_header}")
file(MAKE_DIRECTORY "${OPEN_LIST_BINARY_DIR}")
set(_open_methods "")
foreach(_signature IN ITEMS
    "Bool PathfindCellList::canReverseSort("
    "inline UnsignedInt PathfindCell::getTotalCostDifference("
    "void PathfindCell::forwardInsertionSort("
    "void PathfindCell::reverseInsertionSort("
    "void PathfindCell::putOnSortedOpenList("
    "void PathfindCell::removeFromOpenList("
    "Int PathfindCell::releaseOpenList(")
    string(FIND "${_open_source}" "${_signature}" _start)
    if(_start LESS 0)
        message(FATAL_ERROR "Missing live open-list method: ${_signature}")
    endif()
    string(SUBSTRING "${_open_source}" ${_start} -1 _suffix)
    # Production method closing braces occupy column zero; nested ones are indented.
    string(FIND "${_suffix}" "\n}\n" _end)
    if(_end LESS 0)
        message(FATAL_ERROR "Missing open-list method boundary: ${_signature}")
    endif()
    math(EXPR _length "${_end} + 3")
    string(SUBSTRING "${_suffix}" 0 ${_length} _method)
    string(APPEND _open_methods "${_method}\n")
endforeach()
file(WRITE "${OPEN_LIST_BINARY_DIR}/OpenListMethods.inc" "${_open_methods}")
set(_open_accessors "")
foreach(_accessor IN ITEMS getNextOpen getPrevOpen getTotalCost setTotalCost)
    string(REGEX MATCH "[^\n]*${_accessor}\\([^\n]*\\)[^\n]*\\{[^\n]*" _line "${_open_header}")
    if(NOT _line)
        message(FATAL_ERROR "Missing live open-list accessor: ${_accessor}")
    endif()
    string(APPEND _open_accessors "${_line}\n")
endforeach()
file(WRITE "${OPEN_LIST_BINARY_DIR}/OpenListAccessors.inc" "${_open_accessors}")
string(REGEX MATCH "[^\n]*UnsignedShort m_totalCost, m_costSoFar;[^\n]*" _costs "${_open_header}")
if(NOT _costs)
    message(FATAL_ERROR "Open-list fixture requires live UnsignedShort cost storage")
endif()
file(WRITE "${OPEN_LIST_BINARY_DIR}/OpenListCosts.inc" "${_costs}\n")
if(NOT CMAKE_SCRIPT_MODE_FILE)
    add_executable(core_path_open_list_tests "${CMAKE_CURRENT_LIST_DIR}/OpenListInsertionTest.cpp")
    target_include_directories(core_path_open_list_tests PRIVATE "${OPEN_LIST_BINARY_DIR}")
    # Retail-compatible insertion returns before this fixed-mode dispatch.
    target_compile_definitions(core_path_open_list_tests PRIVATE RETAIL_COMPATIBLE_PATHFINDING=0)
    set_target_properties(core_path_open_list_tests PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    add_test(NAME core_path_open_list_tests COMMAND core_path_open_list_tests)
    set_tests_properties(core_path_open_list_tests PROPERTIES TIMEOUT 30)
endif()
