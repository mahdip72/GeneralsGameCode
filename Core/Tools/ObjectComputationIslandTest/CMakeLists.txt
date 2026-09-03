if(NOT IS_VS6_BUILD)
    # CI checks valid execution and owner fallback, not physical scaling.
    # Invoke either tool with --strict-scaling for the separate strict lane.
    add_executable(core_object_computation_island_tests
        ObjectComputationIslandTest.cpp
    )
    target_link_libraries(core_object_computation_island_tests PRIVATE
        core_task_runtime
    )
    set_target_properties(core_object_computation_island_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
    add_test(NAME core_object_computation_island_tests
        COMMAND core_object_computation_island_tests --allow-serial-fallback
    )
    set_tests_properties(core_object_computation_island_tests PROPERTIES
        TIMEOUT 30
        LABELS correctness
    )

    add_executable(core_object_computation_island_benchmark
        ObjectComputationIslandBenchmark.cpp
    )
    target_link_libraries(core_object_computation_island_benchmark PRIVATE
        core_task_runtime
    )
    set_target_properties(core_object_computation_island_benchmark PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
    add_test(NAME core_object_computation_island_benchmark
        COMMAND core_object_computation_island_benchmark --allow-serial-fallback
    )
    set_tests_properties(core_object_computation_island_benchmark PROPERTIES
        TIMEOUT 120
        LABELS correctness
    )
endif()

add_executable(core_object_computation_island_legacy_header_test
    ObjectComputationIslandLegacyHeaderTest.cpp
)
target_link_libraries(core_object_computation_island_legacy_header_test PRIVATE
    core_task_runtime
)
set_target_properties(core_object_computation_island_legacy_header_test PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
add_test(NAME core_object_computation_island_legacy_header_test
    COMMAND core_object_computation_island_legacy_header_test
)
set_tests_properties(core_object_computation_island_legacy_header_test PROPERTIES
    TIMEOUT 30
)

if(WIN32)
    add_test(NAME core_horde_object_computation_title_contract
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/HordeObjectComputationTitleContract.Tests.ps1"
            -SourceRoot "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(core_horde_object_computation_title_contract PROPERTIES
        TIMEOUT 30
    )
endif()
