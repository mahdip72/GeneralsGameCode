add_executable(core_parallel_visibility_tests ParallelVisibilityTest.cpp)
target_link_libraries(core_parallel_visibility_tests PRIVATE core_task_runtime)

if(RTS_BUILD_CORE_EXTRAS)
    target_compile_definitions(core_parallel_visibility_tests PRIVATE RTS_BUILD_CORE_EXTRAS)
endif()

if(IS_VS6_BUILD)
    set_target_properties(core_parallel_visibility_tests PROPERTIES CXX_STANDARD 98)
else()
    set_target_properties(core_parallel_visibility_tests PROPERTIES CXX_STANDARD 20)
endif()
set_target_properties(core_parallel_visibility_tests PROPERTIES
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
if(MINGW)
    target_link_options(core_parallel_visibility_tests PRIVATE -mconsole)
endif()

add_test(NAME core_parallel_visibility_tests COMMAND core_parallel_visibility_tests)
set_tests_properties(core_parallel_visibility_tests PROPERTIES TIMEOUT 60)
add_test(NAME core_parallel_visibility_serial_policy_test
    COMMAND core_parallel_visibility_tests --serial-policy)
set_tests_properties(core_parallel_visibility_serial_policy_test PROPERTIES TIMEOUT 30)
