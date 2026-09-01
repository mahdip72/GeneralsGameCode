add_library(g_xcrc OBJECT
    "${CMAKE_SOURCE_DIR}/Core/Tools/XferCrcSnapshotTest/XferCrcSnapshotTest.cpp"
)
target_include_directories(g_xcrc PRIVATE
    "${CMAKE_SOURCE_DIR}/Core/Tools/XferCrcSnapshotTest"
)
target_link_libraries(g_xcrc PRIVATE
    g_gameengine
    gi_always
)
set_target_properties(g_xcrc PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)

add_executable(g_skirmish_ai_runner_contract_tests)

target_sources(g_skirmish_ai_runner_contract_tests PRIVATE
    SkirmishAITestRunnerContractTest.cpp
    $<TARGET_OBJECTS:g_xcrc>
)
target_include_directories(g_skirmish_ai_runner_contract_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/Core/Tools/XferCrcSnapshotTest"
)

target_link_libraries(g_skirmish_ai_runner_contract_tests PRIVATE
    comctl32
    core_debug
    core_profile_legacy
    imm32
    rts_legacy_product_runtime
    vfw32
    winmm
    g_gameengine
    g_gameenginedevice
    gi_always
)

add_test(NAME g_skirmish_ai_runner_contract_tests
    COMMAND g_skirmish_ai_runner_contract_tests)
set_tests_properties(g_skirmish_ai_runner_contract_tests PROPERTIES TIMEOUT 30)

add_test(NAME g_xfer_crc_snapshot_tests
    COMMAND g_skirmish_ai_runner_contract_tests --xfer-crc-snapshot)
set_tests_properties(g_xfer_crc_snapshot_tests PROPERTIES TIMEOUT 30)
