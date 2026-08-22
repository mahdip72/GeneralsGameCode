add_executable(core_version_handle_contract_tests
    VersionHandleContractTest.cpp
)

target_include_directories(core_version_handle_contract_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas"
)

set_target_properties(core_version_handle_contract_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_version_handle_contract_tests
    COMMAND core_version_handle_contract_tests
)
set_tests_properties(core_version_handle_contract_tests PROPERTIES TIMEOUT 30)
