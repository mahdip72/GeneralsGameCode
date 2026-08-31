add_executable(core_replay_field_reader_tests
    ReplayFieldReaderTest.cpp
)

target_include_directories(core_replay_field_reader_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
)

target_link_libraries(core_replay_field_reader_tests PRIVATE
    core_runtime_epoch_contract
)

set_target_properties(core_replay_field_reader_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_replay_field_reader_tests COMMAND core_replay_field_reader_tests)
set_tests_properties(core_replay_field_reader_tests PROPERTIES TIMEOUT 30)
