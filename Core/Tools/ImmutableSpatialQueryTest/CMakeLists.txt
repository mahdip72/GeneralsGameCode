cmake_minimum_required(VERSION 3.20)
project(ImmutableSpatialQueryIsolatedTest LANGUAGES CXX)

enable_testing()

add_library(immutable_spatial_query_core STATIC
    ../../Libraries/Source/TaskRuntime/ImmutableSpatialQuery.cpp
)
target_include_directories(immutable_spatial_query_core PUBLIC
    ../../Libraries/Include
)
set_target_properties(immutable_spatial_query_core PROPERTIES
    # MSVC has no modern /std:c++98 switch. The source is also compiled
    # directly with VC6 /Y- in the focused compatibility gate.
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    DISABLE_PRECOMPILE_HEADERS YES
)

add_executable(immutable_spatial_query_tests
    ImmutableSpatialQueryTest.cpp
)
target_link_libraries(immutable_spatial_query_tests PRIVATE
    immutable_spatial_query_core
)
set_target_properties(immutable_spatial_query_tests PROPERTIES
    # The harness intentionally uses std::thread for real concurrent ranges;
    # it is not presented as a C++98 test executable.
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

if(MSVC)
    target_compile_options(immutable_spatial_query_core PRIVATE /W4)
    target_compile_options(immutable_spatial_query_tests PRIVATE /W4)
else()
    target_compile_options(immutable_spatial_query_core PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_options(immutable_spatial_query_tests PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()

add_test(NAME immutable_spatial_query_tests
    COMMAND immutable_spatial_query_tests
)
set_tests_properties(immutable_spatial_query_tests PROPERTIES TIMEOUT 30)
