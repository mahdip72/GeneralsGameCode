add_executable(core_file_transfer_timeout_tests
    FileTransferTimeoutTest.cpp
)

target_include_directories(core_file_transfer_timeout_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/Core/Libraries/Include"
    "${CMAKE_SOURCE_DIR}/Dependencies/Utility"
)

set_target_properties(core_file_transfer_timeout_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_file_transfer_timeout_tests
    COMMAND core_file_transfer_timeout_tests
)
set_tests_properties(core_file_transfer_timeout_tests PROPERTIES TIMEOUT 30)
