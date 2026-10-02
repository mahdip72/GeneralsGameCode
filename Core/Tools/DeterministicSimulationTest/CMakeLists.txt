add_executable(core_deterministic_simulation_tests DeterministicSimulationTest.cpp)
target_link_libraries(core_deterministic_simulation_tests PRIVATE core_task_runtime)
if(RTS_BUILD_CORE_EXTRAS AND NOT IS_VS6_BUILD)
    target_compile_definitions(core_deterministic_simulation_tests PRIVATE
        RTS_BUILD_CORE_EXTRAS
    )
endif()
set_target_properties(core_deterministic_simulation_tests PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
if(MINGW)
    target_link_options(core_deterministic_simulation_tests PRIVATE -mconsole)
endif()

add_test(NAME core_deterministic_simulation_test
    COMMAND core_deterministic_simulation_tests)
set_tests_properties(core_deterministic_simulation_test PROPERTIES TIMEOUT 30)
if(RTS_BUILD_CORE_EXTRAS AND NOT IS_VS6_BUILD)
    add_test(NAME core_deterministic_simulation_parallel_startup_failure_test
        COMMAND core_deterministic_simulation_tests startup-failure parallel)
    add_test(NAME core_deterministic_simulation_shadow_startup_failure_test
        COMMAND core_deterministic_simulation_tests startup-failure shadow)
    set_tests_properties(
        core_deterministic_simulation_parallel_startup_failure_test
        core_deterministic_simulation_shadow_startup_failure_test
        PROPERTIES TIMEOUT 30
    )
endif()
