add_executable(core_network_io_owner_tests NetworkIoOwnerTest.cpp)
target_link_libraries(core_network_io_owner_tests PRIVATE
    core_network_io
    core_task_runtime
    ws2_32
)
set_target_properties(core_network_io_owner_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
if(MINGW)
    target_link_options(core_network_io_owner_tests PRIVATE -mconsole)
endif()

add_test(NAME core_network_io_owner_tests COMMAND core_network_io_owner_tests)
set_tests_properties(core_network_io_owner_tests PROPERTIES TIMEOUT 60)
