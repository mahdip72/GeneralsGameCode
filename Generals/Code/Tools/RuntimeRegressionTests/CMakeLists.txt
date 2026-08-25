add_executable(g_skirmish_ai_runner_contract_tests)

target_sources(g_skirmish_ai_runner_contract_tests PRIVATE
    SkirmishAITestRunnerContractTest.cpp
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
