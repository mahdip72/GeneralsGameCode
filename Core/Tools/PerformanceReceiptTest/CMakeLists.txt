add_executable(core_performance_receipt_tests)

target_sources(core_performance_receipt_tests PRIVATE
    PerformanceReceiptTest.cpp
)

target_link_libraries(core_performance_receipt_tests PRIVATE
    core_task_runtime
)

set_target_properties(core_performance_receipt_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

if(MSVC)
    target_compile_options(core_performance_receipt_tests PRIVATE /EHsc)
endif()

add_test(NAME core_performance_receipt_tests
    COMMAND core_performance_receipt_tests)
set_tests_properties(core_performance_receipt_tests PROPERTIES TIMEOUT 30)

if(WIN32)
    # This graph audit is intentionally colocated with the native-only test:
    # it checks the source is absent from the common/VC6 path and present only
    # in the explicit x64 target_sources block.
    add_test(NAME core_performance_receipt_build_graph_audit
        COMMAND "${CMAKE_COMMAND}"
            -DRTS_SOURCE_ROOT=${CMAKE_SOURCE_DIR}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/PerformanceReceiptBuildGraphTest.cmake")
    set_tests_properties(core_performance_receipt_build_graph_audit
        PROPERTIES TIMEOUT 30)
endif()
