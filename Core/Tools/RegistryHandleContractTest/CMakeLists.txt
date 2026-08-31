add_executable(core_registry_handle_contract_tests
    RegistryHandleContractTest.cpp
)

target_include_directories(core_registry_handle_contract_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/Dependencies/Utility"
    "${CMAKE_SOURCE_DIR}/Core/Libraries/Include"
    "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas"
)

set_target_properties(core_registry_handle_contract_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_registry_handle_contract_tests
    COMMAND core_registry_handle_contract_tests
)
set_tests_properties(core_registry_handle_contract_tests PROPERTIES TIMEOUT 30)
