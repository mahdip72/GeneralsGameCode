add_executable(g_deterministic_ai_planning_tests
    GeneralsDeterministicAIPlanningTest.cpp)

target_include_directories(g_deterministic_ai_planning_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Generals/Code/GameEngine/Include
    ${CMAKE_SOURCE_DIR}/Core/GameEngine/Include
    ${CMAKE_SOURCE_DIR}/Generals/Code/Libraries/Source/WWVegas
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas)
target_link_libraries(g_deterministic_ai_planning_tests PRIVATE
    core_task_runtime)
set_target_properties(g_deterministic_ai_planning_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO)

add_test(NAME g_deterministic_ai_planning_tests
    COMMAND g_deterministic_ai_planning_tests)
set_tests_properties(g_deterministic_ai_planning_tests PROPERTIES TIMEOUT 30)
