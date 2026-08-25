add_executable(core_thread_handle_contract_tests
    ThreadHandleContractTest.cpp
)

target_include_directories(core_thread_handle_contract_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/Dependencies/Utility"
    "${CMAKE_SOURCE_DIR}/Core/Libraries/Include"
    "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas"
)

target_link_libraries(core_thread_handle_contract_tests PRIVATE
    core_wwlib
    winmm
)

set_target_properties(core_thread_handle_contract_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_thread_handle_contract_tests
    COMMAND core_thread_handle_contract_tests
)
set_tests_properties(core_thread_handle_contract_tests PROPERTIES TIMEOUT 30)
