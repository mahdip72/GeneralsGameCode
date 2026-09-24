add_executable(core_kernel_performance_diagnostics_tests
    KernelPerformanceDiagnosticsTest.cpp
)
target_link_libraries(core_kernel_performance_diagnostics_tests PRIVATE core_task_runtime)
set_target_properties(core_kernel_performance_diagnostics_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
if(MSVC)
    target_compile_options(core_kernel_performance_diagnostics_tests PRIVATE /EHsc)
endif()
add_test(NAME core_kernel_performance_diagnostics_tests
    COMMAND core_kernel_performance_diagnostics_tests)
set_tests_properties(core_kernel_performance_diagnostics_tests PROPERTIES TIMEOUT 30)
