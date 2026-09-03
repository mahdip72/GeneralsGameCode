if(WIN32)
    # Keep script-test fixtures beside the configured build tree. The workflow
    # places local build trees on the task-owned H: volume, while hosted CI can
    # retain its normal runner workspace without embedding a machine path.
    set(_stage5_validation_test_scratch_root
        "${CMAKE_CURRENT_BINARY_DIR}/stage5-validation-test-scratch")
    if(MSVC AND NOT IS_VS6_BUILD)
        add_executable(core_validation_process_lifecycle_fixture
            ValidationProcessLifecycleFixture.cpp)
        set_property(TARGET core_validation_process_lifecycle_fixture PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        add_test(NAME core_validation_process_lifecycle_tests
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/InvokeValidationProcessLifecycle.Tests.ps1"
                -FixtureExecutable "$<TARGET_FILE:core_validation_process_lifecycle_fixture>"
                -ScratchRoot "${_stage5_validation_test_scratch_root}")
        set_tests_properties(core_validation_process_lifecycle_tests PROPERTIES
            RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 120)
    endif()
    foreach(_stage5_partition IN ITEMS Plan Runtime Acceptance)
        string(TOLOWER "${_stage5_partition}" _stage5_partition_name)
        add_test(NAME core_deterministic_simulation_validation_${_stage5_partition_name}_tests
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/DeterministicSimulationValidation.Tests.ps1"
                -ValidationPartition "${_stage5_partition}")
        set_tests_properties(core_deterministic_simulation_validation_${_stage5_partition_name}_tests
            PROPERTIES RUN_SERIAL TRUE PROCESSORS 1
                ENVIRONMENT
                    "RTS_STAGE5_VALIDATION_SCRATCH_ROOT=${_stage5_validation_test_scratch_root}")
    endforeach()
    add_test(NAME core_stage5_validation_runner_safety_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5ValidationRunnerSafety.Tests.ps1"
    )
    add_test(NAME core_stage5_performance_scaling_host_validation_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5PerformanceScalingValidation.Tests.ps1"
    )
    add_test(NAME core_replay_mode_propagation_audit_self_test
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Audit-ReplayModePropagation.ps1"
            -SelfTest
    )
    add_test(NAME core_replay_mode_propagation_audit
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Audit-ReplayModePropagation.ps1"
            -SourceRoot "${CMAKE_SOURCE_DIR}"
    )
    if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT IS_VS6_BUILD)
        add_test(NAME core_installed_lockstep_v2_host_self_test
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/Invoke-InstalledLockstepV2Validation.ps1"
                -SelfTest
        )
        set_tests_properties(core_installed_lockstep_v2_host_self_test
            PROPERTIES TIMEOUT 30)
    endif()
    # Keep all assertions and independent evidence rereads. Separate measured
    # Windows PowerShell 5.1 groups (about 42 / 18 / 172 seconds) instead of
    # extending the aggregate test's repeatedly exhausted 300-second budget.
    set_tests_properties(core_deterministic_simulation_validation_plan_tests
        PROPERTIES TIMEOUT 120)
    set_tests_properties(core_deterministic_simulation_validation_runtime_tests
        PROPERTIES TIMEOUT 90)
    set_tests_properties(core_deterministic_simulation_validation_acceptance_tests
        PROPERTIES TIMEOUT 300)
    set_tests_properties(core_stage5_validation_runner_safety_tests
        PROPERTIES TIMEOUT 60
            ENVIRONMENT
                "RTS_STAGE5_VALIDATION_SCRATCH_ROOT=${_stage5_validation_test_scratch_root}")
    set_tests_properties(core_stage5_performance_scaling_host_validation_tests
        PROPERTIES TIMEOUT 300
            ENVIRONMENT
                "RTS_STAGE5_VALIDATION_SCRATCH_ROOT=${_stage5_validation_test_scratch_root}")
    set_tests_properties(
        core_replay_mode_propagation_audit_self_test
        core_replay_mode_propagation_audit
        PROPERTIES TIMEOUT 60
            ENVIRONMENT
                "RTS_STAGE5_VALIDATION_SCRATCH_ROOT=${_stage5_validation_test_scratch_root}")
endif()
