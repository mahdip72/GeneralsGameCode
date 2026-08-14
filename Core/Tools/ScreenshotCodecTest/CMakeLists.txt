add_executable(core_screenshot_codec_tests)

target_sources(core_screenshot_codec_tests PRIVATE
    ScreenshotCodecTest.cpp
    ../../GameEngineDevice/Source/W3DDevice/GameClient/W3DScreenshotCodec.cpp
)

target_include_directories(core_screenshot_codec_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Include
)

target_link_libraries(core_screenshot_codec_tests PRIVATE
    core_task_runtime
)

set_target_properties(core_screenshot_codec_tests PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

if(MINGW)
    target_link_options(core_screenshot_codec_tests PRIVATE -mconsole)
endif()

add_test(NAME core_screenshot_codec_tests COMMAND core_screenshot_codec_tests)
set_tests_properties(core_screenshot_codec_tests PROPERTIES TIMEOUT 30)
