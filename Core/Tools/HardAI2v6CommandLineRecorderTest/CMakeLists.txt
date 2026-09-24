include(HardAI2v6CommandLineRecorderTest.cmake)

if(RTS_BUILD_GENERALS)
    add_executable(g_hard_ai_2v6_cli_recorder_tests)
    set_target_properties(g_hard_ai_2v6_cli_recorder_tests PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    _hard_ai_2v6_add_title_test(g_hard_ai_2v6_cli_recorder_tests Generals
        g_hard_ai_2v6_cli_recorder_tests)
endif()
if(RTS_BUILD_ZEROHOUR)
    add_executable(z_hard_ai_2v6_cli_recorder_tests)
    set_target_properties(z_hard_ai_2v6_cli_recorder_tests PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    _hard_ai_2v6_add_title_test(z_hard_ai_2v6_cli_recorder_tests GeneralsMD
        z_hard_ai_2v6_cli_recorder_tests)
endif()
