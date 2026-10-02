add_executable(core_stage5_performance_fixture_tests Stage5PerformanceFixtureTest.cpp)
target_include_directories(core_stage5_performance_fixture_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/Core/GameEngine/Include"
    "${CMAKE_CURRENT_BINARY_DIR}"
)

# Compile the real map portability and startup gate code against bounded stores.
function(stage5_map_test_extract source start_marker end_marker output)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
    file(READ "${source}" body)
    string(REPLACE "\r\n" "\n" body "${body}")
    string(FIND "${body}" "${start_marker}" begin)
    if(begin LESS 0)
        message(FATAL_ERROR "Stage 5 map test start boundary is missing: ${source}")
    endif()
    string(SUBSTRING "${body}" ${begin} -1 body)
    string(FIND "${body}" "${end_marker}" end)
    if(end LESS 0)
        message(FATAL_ERROR "Stage 5 map test end boundary is missing: ${source}")
    endif()
    string(SUBSTRING "${body}" 0 ${end} body)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${output}" CONTENT "${body}")
endfunction()
stage5_map_test_extract(
    "${CMAKE_SOURCE_DIR}/GeneralsMD/Code/GameEngine/Source/Common/System/SaveGame/GameState.cpp"
    "static const char* findLastBackslashInRangeInclusive" "Bool GameState::doesSaveGameExist"
    "Stage5MapPathCodeUnderTest.inc")
stage5_map_test_extract(
    "${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/GameNetwork/GameInfo.cpp"
    "AsciiString mapName = game->getMap();" "AsciiString optionsString;"
    "Stage5MapOptionsCodeUnderTest.inc")
stage5_map_test_extract(
    "${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/Common/Stage5PerformanceFixtureRunner.cpp"
    "// Map identity and content gate" "delete TheSkirmishGameInfo;"
    "Stage5MapGateUnderTest.inc")
stage5_map_test_extract(
    "${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/Common/Stage5PerformanceFixtureRunner.cpp"
    "bool ResolveStage5MapIdentity(" "Bool ConfigureStage5PerformanceFixture("
    "Stage5MapResolverUnderTest.inc")
stage5_map_test_extract(
    "${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/Common/Stage5PerformanceFixtureRunner.cpp"
    "bool HashVirtualMap(" "unsigned CountRosterPlayers("
    "Stage5MapHashUnderTest.inc")
stage5_map_test_extract(
    "${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/Common/Stage5PerformanceFixtureRunner.cpp"
    "TheSkirmishGameInfo->setMap(" "TheWritableGlobalData->m_useFpsLimit"
    "Stage5MapLoadUnderTest.inc")
set_target_properties(core_stage5_performance_fixture_tests PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
)
add_test(NAME core_stage5_performance_fixture_tests COMMAND core_stage5_performance_fixture_tests)
set_tests_properties(core_stage5_performance_fixture_tests PROPERTIES TIMEOUT 30)

# Exercise the actual owner snapshot boundary without starting either game.
set(_phase_capture_source "${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/Common/SkirmishAITestRunner.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_phase_capture_source}")
file(READ "${_phase_capture_source}" _phase_capture_text)
string(REPLACE "\r\n" "\n" _phase_capture_text "${_phase_capture_text}")
string(FIND "${_phase_capture_text}" "void CaptureSkirmishAITestRuntimeState()\n{" _phase_capture_start)
if(_phase_capture_start LESS 0)
    message(FATAL_ERROR "Skirmish phase snapshot capture function is missing")
endif()
string(SUBSTRING "${_phase_capture_text}" ${_phase_capture_start} -1 _phase_capture_body)
string(FIND "${_phase_capture_body}" "\n#if defined(_WIN64)\nstruct ClosePerformanceMapFile" _phase_capture_end)
if(_phase_capture_end LESS 0)
    message(FATAL_ERROR "Skirmish phase snapshot capture end marker is missing")
endif()
string(SUBSTRING "${_phase_capture_body}" 0 ${_phase_capture_end} _phase_capture_body)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/SkirmishPhaseCaptureUnderTest.inc"
    CONTENT "${_phase_capture_body}")
add_executable(core_skirmish_phase_snapshot_tests SkirmishPhaseSnapshotTest.cpp)
target_include_directories(core_skirmish_phase_snapshot_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(core_skirmish_phase_snapshot_tests PRIVATE simulation_phase_graph_owner_adapter_core)
set_target_properties(core_skirmish_phase_snapshot_tests PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
add_test(NAME core_skirmish_phase_snapshot_tests COMMAND core_skirmish_phase_snapshot_tests)
set_tests_properties(core_skirmish_phase_snapshot_tests PROPERTIES TIMEOUT 30)

# Typed reviewed4v2 map command-line binding; independent of game startup.
add_executable(core_reviewed_ai_map_contract_tests ReviewedAiMapContractTest.cpp)
target_include_directories(core_reviewed_ai_map_contract_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/Core/GameEngine/Include")
set_target_properties(core_reviewed_ai_map_contract_tests PROPERTIES
    CXX_STANDARD 98 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
add_test(NAME core_reviewed_ai_map_contract_tests COMMAND core_reviewed_ai_map_contract_tests)
set_tests_properties(core_reviewed_ai_map_contract_tests PROPERTIES TIMEOUT 30)
