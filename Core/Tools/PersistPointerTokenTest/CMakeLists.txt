add_executable(core_persist_pointer_token_tests
    PersistPointerTokenTest.cpp
)

target_include_directories(core_persist_pointer_token_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas"
)

set_target_properties(core_persist_pointer_token_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_persist_pointer_token_tests
    COMMAND core_persist_pointer_token_tests
)
set_tests_properties(core_persist_pointer_token_tests PROPERTIES TIMEOUT 30)
