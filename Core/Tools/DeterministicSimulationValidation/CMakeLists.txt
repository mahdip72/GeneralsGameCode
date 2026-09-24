if(WIN32)
    # Keep GUID-named process fixtures below a short build-root directory.
    # Nesting them under this tools directory exceeds Process.Start's legacy
    # Windows path limit in longer CI presets, before any child can launch.
    # Prefer an explicitly supplied task-owned scratch volume. CI exports the
    # same value before configuring so H:-bound evidence fixtures stay on the
    # task volume; local configurations retain the historical build-root
    # default when neither a cache value nor environment default is provided.
    set(RTS_STAGE5_VALIDATION_SCRATCH_ROOT "" CACHE PATH
        "Task-owned scratch root for Stage 5 validation tests")
    if(RTS_STAGE5_VALIDATION_SCRATCH_ROOT STREQUAL "")
        if(DEFINED ENV{RTS_STAGE5_VALIDATION_SCRATCH_ROOT} AND
                NOT "$ENV{RTS_STAGE5_VALIDATION_SCRATCH_ROOT}" STREQUAL "")
            set(RTS_STAGE5_VALIDATION_SCRATCH_ROOT
                "$ENV{RTS_STAGE5_VALIDATION_SCRATCH_ROOT}" CACHE PATH
                "Task-owned scratch root for Stage 5 validation tests" FORCE)
        else()
            set(RTS_STAGE5_VALIDATION_SCRATCH_ROOT
                "${CMAKE_BINARY_DIR}/s5-validation" CACHE PATH
                "Task-owned scratch root for Stage 5 validation tests" FORCE)
        endif()
    endif()
    set(_stage5_validation_test_scratch_root
        "${RTS_STAGE5_VALIDATION_SCRATCH_ROOT}")
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

        add_executable(core_validation_profile_root_tests
            "${CMAKE_CURRENT_SOURCE_DIR}/../ValidationProfileRootTest/ValidationProfileRootTest.cpp")
        target_include_directories(core_validation_profile_root_tests PRIVATE
            "${CMAKE_SOURCE_DIR}/Core/Libraries/Include")
        target_link_libraries(core_validation_profile_root_tests PRIVATE advapi32)
        set_target_properties(core_validation_profile_root_tests PROPERTIES
            CXX_STANDARD 98 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
        add_test(NAME core_validation_profile_root_tests
            COMMAND core_validation_profile_root_tests)
        set_tests_properties(core_validation_profile_root_tests PROPERTIES
            RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 60)
    endif()
    add_test(NAME core_stage5_validation_profile_root_contract_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/ValidationProfileRootContract.Tests.ps1"
            -ScratchRoot "${_stage5_validation_test_scratch_root}/profile-contract")
    set_tests_properties(core_stage5_validation_profile_root_contract_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 120)
    add_test(NAME core_stage5_powershell_module_host_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5PowerShellModuleHost.Tests.ps1")
    set_tests_properties(core_stage5_powershell_module_host_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 60)
    add_test(NAME core_stage5_validation_partition_selection_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5ValidationPartitionSelection.Tests.ps1")
    set_tests_properties(core_stage5_validation_partition_selection_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 60)
    add_test(NAME core_stage5_acceptance_corpus_reuse_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5AcceptanceCorpusReuse.Tests.ps1"
            -ScratchRoot "${_stage5_validation_test_scratch_root}/acceptance-corpus-reuse")
    set_tests_properties(core_stage5_acceptance_corpus_reuse_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 60)
    foreach(_stage5_partition IN ITEMS Plan Runtime Acceptance)
        string(TOLOWER "${_stage5_partition}" _stage5_partition_name)
        set(_stage5_partition_verbosity_args)
        if(_stage5_partition STREQUAL "Acceptance")
            # Expose bounded Write-Verbose phase and receipt progress from the
            # long acceptance matrix in CI, including its CTest timeout output.
            list(APPEND _stage5_partition_verbosity_args -Verbose)
        endif()
        add_test(NAME core_deterministic_simulation_validation_${_stage5_partition_name}_tests
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/DeterministicSimulationValidation.Tests.ps1"
                -ValidationPartition "${_stage5_partition}"
                ${_stage5_partition_verbosity_args})
        set_tests_properties(core_deterministic_simulation_validation_${_stage5_partition_name}_tests
            PROPERTIES RUN_SERIAL TRUE PROCESSORS 1
                ENVIRONMENT
                    "RTS_STAGE5_VALIDATION_SCRATCH_ROOT=${_stage5_validation_test_scratch_root}")
    endforeach()
    # The complete mixed native-path producer uses a fresh two-title,
    # 253-child corpus. Run it as a separate bounded acceptance gate so its
    # independent immutable-read/staging pass has its own CTest budget.
    add_test(NAME core_deterministic_simulation_validation_combined_native_path_modes_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/DeterministicSimulationValidation.Tests.ps1"
            -ValidationPartition Acceptance
            -FocusedAcceptanceCase CombinedNativePathModes
            -Verbose)
    set_tests_properties(
        core_deterministic_simulation_validation_combined_native_path_modes_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 7200
            LABELS stage5-extended
            ENVIRONMENT
                "RTS_STAGE5_VALIDATION_SCRATCH_ROOT=${_stage5_validation_test_scratch_root}")
    add_test(NAME core_stage5_replay_corpus_exporter_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5ReplayCorpusExporter.Tests.ps1"
            -ScratchRoot "${_stage5_validation_test_scratch_root}/exporter")
    set_tests_properties(core_stage5_replay_corpus_exporter_tests PROPERTIES
        RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 300)
    add_test(NAME core_stage5_validation_runner_safety_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5ValidationRunnerSafety.Tests.ps1"
    )
    add_test(NAME core_stage5_validation_results_provenance_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5ValidationResultsProvenance.Tests.ps1"
            -ScratchRoot "${_stage5_validation_test_scratch_root}/results-provenance"
    )
    add_test(NAME core_stage5_final_acceptance_native_relocation_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5FinalAcceptanceNativeRelocation.Tests.ps1"
            -ScratchRoot "${_stage5_validation_test_scratch_root}/native-relocation"
    )
    set_tests_properties(core_stage5_final_acceptance_native_relocation_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 60)
    add_test(NAME core_stage5_development_readiness_raw_index_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5DevelopmentReadinessRawLog.Tests.ps1"
            -ScratchRoot "${_stage5_validation_test_scratch_root}/raw-index"
    )
    set_tests_properties(core_stage5_development_readiness_raw_index_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 60)
    add_test(NAME core_stage5_schema_version_type_contract_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5SchemaVersionTypeContract.Tests.ps1"
            -SourceRoot "${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME core_stage5_schema_version_scalar_guard_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5SchemaVersionScalarGuard.Tests.ps1"
            -SourceRoot "${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME core_stage5_schema_version_schema_contract_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5SchemaVersionSchemaContract.Tests.ps1"
            -SourceRoot "${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME core_stage5_json_array_cardinality_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5JsonArrayCardinality.Tests.ps1"
            -SourceRoot "${CMAKE_SOURCE_DIR}"
            -ScratchRoot "${_stage5_validation_test_scratch_root}"
    )
    set_tests_properties(core_stage5_json_array_cardinality_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 60)
    add_test(NAME core_stage5_lockstep_numeric_type_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5LockstepNumericType.Tests.ps1"
            -ScratchRoot "${_stage5_validation_test_scratch_root}/lockstep-numeric"
    )
    set_tests_properties(core_stage5_lockstep_numeric_type_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 600)
    add_test(NAME core_stage5_performance_qualification_data_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/New-Stage5PerformanceQualificationData.Tests.ps1"
            -ScratchRoot "${_stage5_validation_test_scratch_root}/performance-qualification-data")
    set_tests_properties(core_stage5_performance_qualification_data_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 300)
    add_test(NAME core_stage5_combined_host_runner_receipt_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5CombinedHostRunnerReceipt.Tests.ps1"
            -SourceRoot "${CMAKE_SOURCE_DIR}"
            -ScratchRoot "${_stage5_validation_test_scratch_root}")
    set_tests_properties(core_stage5_combined_host_runner_receipt_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 120)
    add_test(NAME core_stage5_development_readiness_bundle_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5DevelopmentReadinessBundle.Tests.ps1"
            -SourceRoot "${CMAKE_SOURCE_DIR}")
    set_tests_properties(core_stage5_development_readiness_bundle_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 120)
    # These fixtures exercise JSON Schema validation through Test-Json.  Keep
    # their PowerShell 7 host explicit; Windows PowerShell 5.1 has no native
    # Test-Json and must not silently turn schema validation into a no-op.
    add_test(NAME core_stage5_installed_kernel_execution_tests
        COMMAND pwsh -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5InstalledKernelExecution.Tests.ps1"
            -ScratchRoot "${_stage5_validation_test_scratch_root}/installed-kernel-execution")
    set_tests_properties(core_stage5_installed_kernel_execution_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 180)
    add_test(NAME core_stage5_installed_lockstep_v2_session_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5InstalledLockstepV2Session.Tests.ps1")
    set_tests_properties(core_stage5_installed_lockstep_v2_session_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 60)
    foreach(_stage5_process_host IN ITEMS powershell pwsh)
        add_test(NAME core_stage5_suspended_process_${_stage5_process_host}_tests
            COMMAND ${_stage5_process_host} -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5InstalledLockstepV2Session.Tests.ps1"
                -LifecyclePreflightOnly
                -ScratchRoot "${_stage5_validation_test_scratch_root}")
        set_tests_properties(core_stage5_suspended_process_${_stage5_process_host}_tests
            PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 60)
    endforeach()
    add_test(NAME core_stage5_local_lockstep_diagnostic_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5LocalLockstepDiagnostic.Tests.ps1")
    set_tests_properties(core_stage5_local_lockstep_diagnostic_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 60)
    add_test(NAME core_stage5_local_lockstep_diagnostic_receipt_tests
        COMMAND pwsh -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5LocalLockstepDiagnosticReceipt.Tests.ps1"
            -ScratchRoot "${_stage5_validation_test_scratch_root}")
    set_tests_properties(core_stage5_local_lockstep_diagnostic_receipt_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 60)
    add_test(NAME core_stage5_local_lockstep_diagnostic_data_tests
        COMMAND pwsh -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5LocalLockstepDiagnosticData.Tests.ps1"
            -ScratchRoot "${_stage5_validation_test_scratch_root}"
            -RequireSchemaValidation)
    set_tests_properties(core_stage5_local_lockstep_diagnostic_data_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 60)
    add_test(NAME core_stage5_installed_kernel_module_scope_tests
        COMMAND pwsh -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5InstalledKernelExecutionEvidence.Tests.ps1"
            -ModuleScopePreflightOnly)
    set_tests_properties(core_stage5_installed_kernel_module_scope_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 60)
    add_test(NAME core_stage5_installed_kernel_execution_evidence_tests
        COMMAND pwsh -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5InstalledKernelExecutionEvidence.Tests.ps1"
            -ScratchRoot "${_stage5_validation_test_scratch_root}/installed-kernel-evidence")
    set_tests_properties(core_stage5_installed_kernel_execution_evidence_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 300)
    add_test(NAME core_stage5_lockstep_qualification_binding_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5LockstepQualificationBinding.Tests.ps1"
            -ScratchRoot "${_stage5_validation_test_scratch_root}/lockstep-qualification")
    set_tests_properties(core_stage5_lockstep_qualification_binding_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 120)
    add_test(NAME core_stage5_native_performance_fixture_production_tests
        COMMAND pwsh -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5NativePerformanceFixtureProduction.Tests.ps1"
            -ScratchRoot "${_stage5_validation_test_scratch_root}/native-performance-fixture")
    set_tests_properties(core_stage5_native_performance_fixture_production_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 300)
    add_test(NAME core_stage5_native_fixture_diagnostic_tests
        COMMAND pwsh -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5NativeFixtureDiagnostic.Tests.ps1"
            -ScratchRoot "${_stage5_validation_test_scratch_root}/native-fixture-diagnostic")
    set_tests_properties(core_stage5_native_fixture_diagnostic_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 120)
    add_test(NAME core_stage5_native_fixture_diagnostic_lifecycle_tests
        COMMAND pwsh -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5NativeFixtureDiagnosticLifecycle.Tests.ps1"
            -ScratchRoot "${_stage5_validation_test_scratch_root}/native-fixture-diagnostic-lifecycle")
    set_tests_properties(core_stage5_native_fixture_diagnostic_lifecycle_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 180)
    add_test(NAME core_stage5_reviewed_ai_map_tests
        COMMAND pwsh -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5ReviewedAiMap.Tests.ps1"
            -ScratchRoot "${_stage5_validation_test_scratch_root}/reviewed-ai-map")
    set_tests_properties(core_stage5_reviewed_ai_map_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 120)
    add_test(NAME core_stage5_qualification_provisioning_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5QualificationProvisioning.Tests.ps1")
    set_tests_properties(core_stage5_qualification_provisioning_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 60)
    add_test(NAME core_stage5_simulation_qualification_data_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5SimulationQualificationData.Tests.ps1"
            -ScratchRoot "${_stage5_validation_test_scratch_root}/simulation-qualification-data")
    set_tests_properties(core_stage5_simulation_qualification_data_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 300)
    add_test(NAME core_stage5_registry_recovery_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5RegistryRecovery.Tests.ps1"
            -ScratchRoot "${_stage5_validation_test_scratch_root}/registry-recovery")
    set_tests_properties(core_stage5_registry_recovery_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 120)
    foreach(_stage5_binding_test IN ITEMS BaseGeneralsBinding PairedPerformanceBinding SimulationProvisioningRoles)
        add_test(NAME core_stage5_${_stage5_binding_test}_tests
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/Stage5${_stage5_binding_test}.Tests.ps1"
                -ScratchRoot "${_stage5_validation_test_scratch_root}/base-generals-binding")
        set_tests_properties(core_stage5_${_stage5_binding_test}_tests
            PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 120)
    endforeach()
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
    # Keep all assertions and independent evidence rereads. The Acceptance
    # partition reached about 840 seconds on the supported host before its
    # qualification-data fixture defect was isolated; retain a bounded full
    # budget while preserving the complete assertion matrix.
    set_tests_properties(core_deterministic_simulation_validation_plan_tests
        PROPERTIES TIMEOUT 120)
    set_tests_properties(core_deterministic_simulation_validation_runtime_tests
        PROPERTIES TIMEOUT 90)
    set_tests_properties(core_deterministic_simulation_validation_acceptance_tests
        PROPERTIES TIMEOUT 7200)
    set_tests_properties(core_stage5_validation_runner_safety_tests
        PROPERTIES TIMEOUT 60
            ENVIRONMENT
                "RTS_STAGE5_VALIDATION_SCRATCH_ROOT=${_stage5_validation_test_scratch_root}")
    set_tests_properties(core_stage5_validation_results_provenance_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 60)
    set_tests_properties(core_stage5_schema_version_scalar_guard_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 60)
    set_tests_properties(core_stage5_schema_version_schema_contract_tests
        PROPERTIES RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 60)
    set_tests_properties(core_stage5_performance_scaling_host_validation_tests
        # The complete security/cache/path suite was measured at about 2280
        # seconds on the supported 6-core local-capacity host. Keep a bounded
        # 3600-second margin without weakening any assertions.
        PROPERTIES TIMEOUT 3600
            ENVIRONMENT
                "RTS_STAGE5_VALIDATION_SCRATCH_ROOT=${_stage5_validation_test_scratch_root}")
    set_tests_properties(
        core_replay_mode_propagation_audit_self_test
        core_replay_mode_propagation_audit
        PROPERTIES TIMEOUT 60
            ENVIRONMENT
                "RTS_STAGE5_VALIDATION_SCRATCH_ROOT=${_stage5_validation_test_scratch_root}")

    # CTest launches Windows PowerShell directly rather than through a
    # PowerShell host.  When the parent process carries a mixed PS7/PS5
    # PSModulePath, Windows PowerShell 5.1 can import the PS7
    # Microsoft.PowerShell.Utility module first.  That module loads but does
    # not export the Windows PowerShell Get-FileHash command, causing these
    # tests to fail before their source-connected assertions run.  Clear the
    # inherited value for only the Windows PowerShell tests; powershell.exe
    # repopulates its supported PS5 module roots, while pwsh tests retain
    # their normal PS7 module path and schema-validation behavior.
    set_property(TEST
        core_stage5_validation_profile_root_contract_tests
        core_stage5_powershell_module_host_tests
        core_stage5_validation_partition_selection_tests
        core_deterministic_simulation_validation_plan_tests
        core_deterministic_simulation_validation_runtime_tests
        core_deterministic_simulation_validation_acceptance_tests
        core_deterministic_simulation_validation_combined_native_path_modes_tests
        core_stage5_replay_corpus_exporter_tests
        core_stage5_validation_runner_safety_tests
        core_stage5_validation_results_provenance_tests
        core_stage5_final_acceptance_native_relocation_tests
        core_stage5_development_readiness_raw_index_tests
        core_stage5_schema_version_type_contract_tests
        core_stage5_schema_version_scalar_guard_tests
        core_stage5_schema_version_schema_contract_tests
        core_stage5_json_array_cardinality_tests
        core_stage5_lockstep_numeric_type_tests
        core_stage5_performance_qualification_data_tests
        core_stage5_combined_host_runner_receipt_tests
        core_stage5_acceptance_corpus_reuse_tests
        core_stage5_development_readiness_bundle_tests
        core_stage5_installed_lockstep_v2_session_tests
        core_stage5_suspended_process_powershell_tests
        core_stage5_local_lockstep_diagnostic_tests
        core_stage5_lockstep_qualification_binding_tests
        core_stage5_qualification_provisioning_tests
        core_stage5_simulation_qualification_data_tests
        core_stage5_registry_recovery_tests
        core_stage5_BaseGeneralsBinding_tests
        core_stage5_PairedPerformanceBinding_tests
        core_stage5_SimulationProvisioningRoles_tests
        core_stage5_performance_scaling_host_validation_tests
        core_replay_mode_propagation_audit_self_test
        core_replay_mode_propagation_audit
        APPEND PROPERTY ENVIRONMENT "PSModulePath=")
endif()
