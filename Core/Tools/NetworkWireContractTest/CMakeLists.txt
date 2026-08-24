add_executable(core_network_wire_contract_tests
    NetworkWireContractTest.cpp
)

target_link_libraries(core_network_wire_contract_tests PRIVATE
    core_runtime_epoch_contract
)

target_include_directories(core_network_wire_contract_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
    ${CMAKE_SOURCE_DIR}/Dependencies/Utility
)

set_target_properties(core_network_wire_contract_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_network_wire_contract_tests
    COMMAND core_network_wire_contract_tests
)
set_tests_properties(core_network_wire_contract_tests PROPERTIES TIMEOUT 30)
