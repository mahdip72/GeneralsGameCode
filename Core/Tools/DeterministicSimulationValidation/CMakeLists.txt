if(WIN32)
    # Keep script-test fixtures beside the configured build tree. The workflow
    # places local build trees on the task-owned H: volume, while hosted CI can
    # retain its normal runner workspace without embedding a machine path.
    set(_stage5_validation_test_scratch_root
        "${CMAKE_CURRENT_BINARY_DIR}/stage5-validation-test-scratch")
    add_test(NAME core_deterministic_simulation_validation_script_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/DeterministicSimulationValidation.Tests.ps1"
    )
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
    set_tests_properties(core_deterministic_simulation_validation_script_tests
        PROPERTIES TIMEOUT 180
            ENVIRONMENT
                "RTS_STAGE5_VALIDATION_SCRATCH_ROOT=${_stage5_validation_test_scratch_root}")
    set_tests_properties(core_stage5_validation_runner_safety_tests
        PROPERTIES TIMEOUT 60
            ENVIRONMENT
                "RTS_STAGE5_VALIDATION_SCRATCH_ROOT=${_stage5_validation_test_scratch_root}")
    set_tests_properties(core_stage5_performance_scaling_host_validation_tests
        PROPERTIES TIMEOUT 120
            ENVIRONMENT
                "RTS_STAGE5_VALIDATION_SCRATCH_ROOT=${_stage5_validation_test_scratch_root}")
    set_tests_properties(
        core_replay_mode_propagation_audit_self_test
        core_replay_mode_propagation_audit
        PROPERTIES TIMEOUT 60
            ENVIRONMENT
                "RTS_STAGE5_VALIDATION_SCRATCH_ROOT=${_stage5_validation_test_scratch_root}")
endif()
