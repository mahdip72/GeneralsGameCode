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

# Compile the actual title predicates and commit/retarget calls against a
# controlled owner shell. The phase bodies remain fixture-controlled so late
# intake resets can be reproduced without initializing either game runtime.
get_filename_component(_phase_title_repository_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
foreach(_phase_title IN ITEMS Generals GeneralsMD)
    set(_phase_title_source "${_phase_title_repository_root}/${_phase_title}/Code/GameEngine/Source/GameLogic/System/GameLogic.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_phase_title_source}")
    file(READ "${_phase_title_source}" _phase_title_text)
    string(FIND "${_phase_title_text}" "bool GameLogic::validateStage5PhaseGraphCommit(" _phase_title_start)
    string(FIND "${_phase_title_text}" "Bool GameLogic::runOwnerIntakePhase(" _phase_title_end)
    if(_phase_title_start LESS 0 OR _phase_title_end LESS_EQUAL _phase_title_start)
        message(FATAL_ERROR "Cannot locate the ${_phase_title} owner phase contract methods")
    endif()
    math(EXPR _phase_title_length "${_phase_title_end} - ${_phase_title_start}")
    string(SUBSTRING "${_phase_title_text}" ${_phase_title_start} ${_phase_title_length} _phase_title_methods)
    set(_phase_title_generated "${CMAKE_CURRENT_BINARY_DIR}/${_phase_title}")
    file(GENERATE OUTPUT "${_phase_title_generated}/SimulationPhaseGraphOwnerTitleMethods.inc"
        CONTENT "${_phase_title_methods}")
    if(_phase_title STREQUAL "Generals")
        set(_phase_title_target g_simulation_phase_owner_title_frame_tests)
    else()
        set(_phase_title_target z_simulation_phase_owner_title_frame_tests)
    endif()
    add_executable(${_phase_title_target} SimulationPhaseGraphOwnerTitleFrameTest.cpp)
    target_link_libraries(${_phase_title_target} PRIVATE simulation_phase_graph_owner_adapter_core)
    target_include_directories(${_phase_title_target} PRIVATE "${_phase_title_generated}")
    target_compile_definitions(${_phase_title_target} PRIVATE "RTS_PHASE_GRAPH_TITLE_NAME=\"${_phase_title}\"")
    set_target_properties(${_phase_title_target} PROPERTIES
        CXX_STANDARD 98 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    add_test(NAME ${_phase_title_target} COMMAND ${_phase_title_target})
    set_tests_properties(${_phase_title_target} PROPERTIES TIMEOUT 30)
endforeach()
