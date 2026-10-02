if(NOT DEFINED RTS_SOURCE_ROOT OR NOT EXISTS
        "${RTS_SOURCE_ROOT}/Core/Libraries/Source/TaskRuntime/CMakeLists.txt")
    message(FATAL_ERROR "RTS_SOURCE_ROOT must point at the source tree")
endif()

file(READ "${RTS_SOURCE_ROOT}/Core/Libraries/Source/TaskRuntime/CMakeLists.txt"
    RTS_TASK_RUNTIME_CMAKE)
file(READ "${RTS_SOURCE_ROOT}/Core/Tools/CMakeLists.txt"
    RTS_TOOLS_CMAKE)
string(FIND "${RTS_TASK_RUNTIME_CMAKE}"
    "if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT IS_VS6_BUILD)"
    RTS_NATIVE_GUARD)
if(RTS_NATIVE_GUARD LESS 0)
    message(FATAL_ERROR "receipt source has no explicit native-x64 guard")
endif()

string(FIND "${RTS_TASK_RUNTIME_CMAKE}" "PerformanceReceipt.cpp"
    RTS_RECEIPT_SOURCE)
if(RTS_RECEIPT_SOURCE LESS 0)
    message(FATAL_ERROR "receipt source is absent from TaskRuntime graph")
endif()

string(SUBSTRING "${RTS_TASK_RUNTIME_CMAKE}" 0 ${RTS_RECEIPT_SOURCE}
    RTS_RECEIPT_PREFIX)
string(FIND "${RTS_RECEIPT_PREFIX}" "PerformanceReceipt.cpp"
    RTS_UNGUARDED_RECEIPT_SOURCE)
if(NOT RTS_UNGUARDED_RECEIPT_SOURCE LESS 0)
    message(FATAL_ERROR
        "receipt source appears in a common/legacy source list")
endif()

string(SUBSTRING "${RTS_TASK_RUNTIME_CMAKE}" ${RTS_NATIVE_GUARD}
    -1 RTS_NATIVE_RECEIPT_BLOCK)
string(FIND "${RTS_NATIVE_RECEIPT_BLOCK}" "target_sources(core_task_runtime PRIVATE"
    RTS_TARGET_SOURCE)
if(RTS_TARGET_SOURCE LESS 0)
    message(FATAL_ERROR "receipt source is not attached to core_task_runtime")
endif()

# The same explicit native guard must exclude the VC6 branch.  This is a
# static graph proof: a VC6 configure has IS_VS6_BUILD=true and therefore
# cannot enter this block, while the x64 configure satisfies its pointer-width
# predicate.
string(FIND "${RTS_TASK_RUNTIME_CMAKE}"
    "if(IS_VS6_BUILD)\n    target_sources(core_task_runtime PRIVATE\n        JobSystemLegacy.cpp"
    RTS_VC6_BLOCK)
if(RTS_VC6_BLOCK LESS 0)
    message(FATAL_ERROR "VC6 runtime branch could not be located")
endif()

string(FIND "${RTS_TOOLS_CMAKE}"
    "if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT IS_VS6_BUILD)\n        add_subdirectory(PerformanceReceiptTest)"
    RTS_TEST_NATIVE_GUARD)
if(RTS_TEST_NATIVE_GUARD LESS 0)
    message(FATAL_ERROR
        "receipt C++ test is not excluded from VC6/non-x64 tool graphs")
endif()

# The source must not occur in the unconditional common source list. The
# explicit guarded target_sources block is the only permitted occurrence.
string(REGEX MATCHALL "PerformanceReceipt\\.cpp" RTS_RECEIPT_OCCURRENCES
    "${RTS_TASK_RUNTIME_CMAKE}")
list(LENGTH RTS_RECEIPT_OCCURRENCES RTS_RECEIPT_COUNT)
if(NOT RTS_RECEIPT_COUNT EQUAL 1)
    message(FATAL_ERROR
        "receipt source must occur exactly once in the guarded graph")
endif()

message(STATUS "PerformanceReceipt.cpp is native-x64/non-VC6 gated")
