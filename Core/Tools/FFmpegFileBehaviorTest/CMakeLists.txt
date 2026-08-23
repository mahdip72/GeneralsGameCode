find_package(FFMPEG REQUIRED)

if(NOT FFMPEG_EXECUTABLE)
    message(FATAL_ERROR "FFmpegFile behavior tests require the ffmpeg command-line tool from the selected SDK.")
endif()

add_executable(core_ffmpeg_file_behavior_test
    FFmpegFileBehaviorTest.cpp
)

target_link_libraries(core_ffmpeg_file_behavior_test PRIVATE
    rts_ffmpeg_movie_playback
)

set_target_properties(core_ffmpeg_file_behavior_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_precompile_headers(core_ffmpeg_file_behavior_test PRIVATE <type_traits>)

add_test(NAME core_ffmpeg_file_behavior_test
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/RunFFmpegFileBehaviorTest.ps1"
        -FFmpegExecutable "${FFMPEG_EXECUTABLE}"
        -TestExecutable "$<TARGET_FILE:core_ffmpeg_file_behavior_test>"
        -FixturePath "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-file-behavior.mp4"
        -RuntimeDirectory "${FFMPEG_RUNTIME_DIR}"
)
set_tests_properties(core_ffmpeg_file_behavior_test PROPERTIES TIMEOUT 30)
