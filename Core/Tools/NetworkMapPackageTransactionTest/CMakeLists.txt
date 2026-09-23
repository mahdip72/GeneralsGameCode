add_executable(core_network_map_package_transaction_tests
    NetworkMapPackageTransactionTest.cpp
)

target_include_directories(core_network_map_package_transaction_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
)

set_target_properties(core_network_map_package_transaction_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_network_map_package_transaction_tests
    COMMAND core_network_map_package_transaction_tests
)
set_tests_properties(core_network_map_package_transaction_tests PROPERTIES TIMEOUT 30)
