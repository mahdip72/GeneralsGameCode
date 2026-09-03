add_executable(core_deterministic_path_search_tests
    DeterministicPathSearchTest.cpp)
target_link_libraries(core_deterministic_path_search_tests PRIVATE core_task_runtime)
set_target_properties(core_deterministic_path_search_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
if(MSVC)
    target_compile_options(core_deterministic_path_search_tests PRIVATE /UNDEBUG)
else()
    target_compile_options(core_deterministic_path_search_tests PRIVATE -UNDEBUG)
endif()
if(MINGW)
    target_link_options(core_deterministic_path_search_tests PRIVATE -mconsole)
endif()

add_test(NAME core_deterministic_path_search_tests
    COMMAND core_deterministic_path_search_tests)
set_tests_properties(core_deterministic_path_search_tests PROPERTIES TIMEOUT 30)
