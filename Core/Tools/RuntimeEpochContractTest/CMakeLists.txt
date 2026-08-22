add_executable(core_runtime_epoch_contract_tests
    RuntimeEpochContractTest.cpp
)

target_link_libraries(core_runtime_epoch_contract_tests PRIVATE
    core_runtime_epoch_contract
)

set_target_properties(core_runtime_epoch_contract_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_runtime_epoch_contract_tests
    COMMAND core_runtime_epoch_contract_tests
)
set_tests_properties(core_runtime_epoch_contract_tests PROPERTIES TIMEOUT 30)
