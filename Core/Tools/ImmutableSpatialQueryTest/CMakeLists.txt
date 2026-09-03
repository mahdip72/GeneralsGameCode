cmake_minimum_required(VERSION 3.20)
project(ImmutableSpatialQueryIsolatedTest LANGUAGES CXX)

enable_testing()

add_executable(immutable_spatial_query_tests
    ImmutableSpatialQueryTest.cpp
)
target_link_libraries(immutable_spatial_query_tests PRIVATE
    core_task_runtime
)
set_target_properties(immutable_spatial_query_tests PROPERTIES
    # The harness intentionally uses std::thread for real concurrent ranges;
    # it is not presented as a C++98 test executable.
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

if(MSVC)
    target_compile_options(immutable_spatial_query_tests PRIVATE /W4)
else()
    target_compile_options(immutable_spatial_query_tests PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()

add_test(NAME immutable_spatial_query_tests
    COMMAND immutable_spatial_query_tests
)
set_tests_properties(immutable_spatial_query_tests PROPERTIES TIMEOUT 30)
