add_executable(core_model_asset_bytes_tests ModelAssetBytesTest.cpp)
target_link_libraries(core_model_asset_bytes_tests PRIVATE core_config core_task_runtime)
set_target_properties(core_model_asset_bytes_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
if(MINGW)
    target_link_options(core_model_asset_bytes_tests PRIVATE -mconsole)
endif()
add_test(NAME core_model_asset_bytes_tests COMMAND core_model_asset_bytes_tests)
set_tests_properties(core_model_asset_bytes_tests PROPERTIES TIMEOUT 30)
