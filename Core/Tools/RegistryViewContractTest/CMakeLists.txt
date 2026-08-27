add_executable(core_registry_view_contract_tests
    RegistryViewContractTest.cpp
)

target_include_directories(core_registry_view_contract_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/Core/Libraries/Include"
)

target_link_libraries(core_registry_view_contract_tests PRIVATE advapi32 ole32)

set_target_properties(core_registry_view_contract_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_registry_view_contract_tests
    COMMAND core_registry_view_contract_tests
)
set_tests_properties(core_registry_view_contract_tests PROPERTIES TIMEOUT 30)

add_test(NAME core_registry_view_contract_audit
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/RegistryViewContractAudit.ps1"
        -SourceRoot "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(core_registry_view_contract_audit PROPERTIES TIMEOUT 30)

add_test(NAME core_registry_view_contract_audit_self_test
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/RegistryViewContractAudit.ps1"
        -SelfTest
)
set_tests_properties(core_registry_view_contract_audit_self_test PROPERTIES TIMEOUT 30)
