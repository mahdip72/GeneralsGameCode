cmake_minimum_required(VERSION 3.20)
project(SimulationPhaseGraphOwnerAdapterIsolatedTest LANGUAGES CXX)

enable_testing()

add_library(simulation_phase_graph_owner_adapter_core STATIC
    ../../Libraries/Source/TaskRuntime/SimulationPhaseGraph.cpp
    ../../Libraries/Source/TaskRuntime/SimulationPhaseGraphOwnerAdapter.cpp
)
target_include_directories(simulation_phase_graph_owner_adapter_core PUBLIC
    ../../Libraries/Include
    ../../../Dependencies/Utility
)
set_target_properties(simulation_phase_graph_owner_adapter_core PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_executable(simulation_phase_graph_owner_adapter_legacy_compile_test
    SimulationPhaseGraphOwnerAdapterLegacyCompileTest.cpp
)
target_link_libraries(simulation_phase_graph_owner_adapter_legacy_compile_test PRIVATE
    simulation_phase_graph_owner_adapter_core
)
set_target_properties(simulation_phase_graph_owner_adapter_legacy_compile_test PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

option(SIMULATION_PHASE_GRAPH_OWNER_ADAPTER_LEGACY_ONLY
    "Build only the C++98 owner-adapter compile/runtime fixture" OFF)
if(NOT SIMULATION_PHASE_GRAPH_OWNER_ADAPTER_LEGACY_ONLY AND NOT IS_VS6_BUILD)
    add_executable(simulation_phase_graph_owner_adapter_tests
        SimulationPhaseGraphOwnerAdapterTest.cpp
    )
    target_link_libraries(simulation_phase_graph_owner_adapter_tests PRIVATE
        simulation_phase_graph_owner_adapter_core
    )
    set_target_properties(simulation_phase_graph_owner_adapter_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
endif()

if(MSVC)
    target_compile_options(simulation_phase_graph_owner_adapter_core PRIVATE /W4)
    target_compile_options(simulation_phase_graph_owner_adapter_legacy_compile_test PRIVATE /W4)
    if(TARGET simulation_phase_graph_owner_adapter_tests)
        target_compile_options(simulation_phase_graph_owner_adapter_tests PRIVATE /W4)
    endif()
else()
    target_compile_options(simulation_phase_graph_owner_adapter_core PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_options(simulation_phase_graph_owner_adapter_legacy_compile_test PRIVATE
        -Wall -Wextra -Wpedantic
    )
    if(TARGET simulation_phase_graph_owner_adapter_tests)
        target_compile_options(simulation_phase_graph_owner_adapter_tests PRIVATE
            -Wall -Wextra -Wpedantic
        )
    endif()
endif()

add_test(NAME simulation_phase_graph_owner_adapter_legacy_compile_test
    COMMAND simulation_phase_graph_owner_adapter_legacy_compile_test
)
if(TARGET simulation_phase_graph_owner_adapter_tests)
    add_test(NAME simulation_phase_graph_owner_adapter_tests
        COMMAND simulation_phase_graph_owner_adapter_tests
    )
endif()
