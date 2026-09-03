add_executable(core_mixed_worker_multiplayer_policy_tests
    MixedWorkerMultiplayerLoopbackContract.cpp
    MixedWorkerMultiplayerTest.cpp
)
target_link_libraries(core_mixed_worker_multiplayer_policy_tests PRIVATE
    core_task_runtime
)
set_target_properties(core_mixed_worker_multiplayer_policy_tests PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
if(MINGW)
    target_link_options(core_mixed_worker_multiplayer_policy_tests PRIVATE -mconsole)
endif()

add_test(NAME core_mixed_worker_multiplayer_policy_test
    COMMAND core_mixed_worker_multiplayer_policy_tests
)
set_tests_properties(core_mixed_worker_multiplayer_policy_test PROPERTIES TIMEOUT 120)

if(WIN32)
    add_test(NAME core_mixed_worker_multiplayer_source_audit
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/MixedWorkerMultiplayerSourceAudit.ps1
            -SourceRoot ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(core_mixed_worker_multiplayer_source_audit PROPERTIES TIMEOUT 30)

    add_test(NAME core_mixed_worker_multiplayer_source_audit_self_test
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/MixedWorkerMultiplayerSourceAudit.ps1
            -SelfTest
    )
    set_tests_properties(core_mixed_worker_multiplayer_source_audit_self_test PROPERTIES TIMEOUT 30)
endif()
