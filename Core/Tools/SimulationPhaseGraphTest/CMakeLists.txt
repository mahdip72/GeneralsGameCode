cmake_minimum_required(VERSION 3.20)
project(SimulationPhaseGraphIsolatedTest LANGUAGES CXX)

enable_testing()

add_library(simulation_phase_graph_core STATIC
    ../../Libraries/Source/TaskRuntime/SimulationPhaseGraph.cpp
)
target_include_directories(simulation_phase_graph_core PUBLIC
    ../../Libraries/Include
    ../../../Dependencies/Utility
)
set_target_properties(simulation_phase_graph_core PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    DISABLE_PRECOMPILE_HEADERS YES
)

add_executable(simulation_phase_graph_legacy_compile_test
    SimulationPhaseGraphLegacyCompileTest.cpp
)
target_link_libraries(simulation_phase_graph_legacy_compile_test PRIVATE
    simulation_phase_graph_core
)
set_target_properties(simulation_phase_graph_legacy_compile_test PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    DISABLE_PRECOMPILE_HEADERS YES
)

option(SIMULATION_PHASE_GRAPH_LEGACY_ONLY
    "Build only the C++98 production/legacy harness" OFF)
if(NOT SIMULATION_PHASE_GRAPH_LEGACY_ONLY AND NOT IS_VS6_BUILD)
    target_compile_definitions(simulation_phase_graph_core PRIVATE
        RTS_BUILD_CORE_EXTRAS
    )
    add_executable(simulation_phase_graph_tests
        SimulationPhaseGraphTest.cpp
    )
    target_compile_definitions(simulation_phase_graph_tests PRIVATE
        RTS_BUILD_CORE_EXTRAS
    )
    target_link_libraries(simulation_phase_graph_tests PRIVATE
        simulation_phase_graph_core
    )
    if(UNIX)
        find_package(Threads REQUIRED)
        target_link_libraries(simulation_phase_graph_tests PRIVATE
            Threads::Threads
        )
    endif()
    set_target_properties(simulation_phase_graph_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
endif()

if(MSVC)
    target_compile_options(simulation_phase_graph_core PRIVATE /W4)
    target_compile_options(simulation_phase_graph_legacy_compile_test PRIVATE /W4)
    if(TARGET simulation_phase_graph_tests)
        target_compile_options(simulation_phase_graph_tests PRIVATE /W4)
    endif()
else()
    target_compile_options(simulation_phase_graph_core PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_options(simulation_phase_graph_legacy_compile_test PRIVATE
        -Wall -Wextra -Wpedantic
    )
    if(TARGET simulation_phase_graph_tests)
        target_compile_options(simulation_phase_graph_tests PRIVATE
            -Wall -Wextra -Wpedantic
        )
    endif()
endif()

add_test(NAME simulation_phase_graph_legacy_compile_test
    COMMAND simulation_phase_graph_legacy_compile_test
)
if(TARGET simulation_phase_graph_tests)
    add_test(NAME simulation_phase_graph_tests
        COMMAND simulation_phase_graph_tests
    )
    set_tests_properties(simulation_phase_graph_tests PROPERTIES TIMEOUT 30)
endif()
set_tests_properties(simulation_phase_graph_legacy_compile_test
    PROPERTIES TIMEOUT 30)
