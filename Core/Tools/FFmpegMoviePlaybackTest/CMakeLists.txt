find_package(FFMPEG REQUIRED)

if(NOT FFMPEG_EXECUTABLE)
    message(FATAL_ERROR "FFmpeg movie playback tests require the ffmpeg command-line tool from the selected SDK.")
endif()

add_executable(core_ffmpeg_movie_playback_test
    FFmpegMoviePlaybackTest.cpp
)

# Compile the runtime adapter in the same focused graph without linking or
# launching a product executable.  The playback behavior remains device-free;
# this target only catches callback/API integration drift at compile time.
add_library(core_ffmpeg_video_player_compile OBJECT
    ${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Source/VideoDevice/FFmpeg/FFmpegVideoPlayer.cpp
)

target_include_directories(core_ffmpeg_video_player_compile PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Include
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas
    ${CMAKE_SOURCE_DIR}/GeneralsMD/Code/GameEngine/Include
)
target_compile_options(core_ffmpeg_video_player_compile PRIVATE
    /FI${CMAKE_SOURCE_DIR}/Dependencies/Utility/Utility/CppMacros.h
)

target_link_libraries(core_ffmpeg_movie_playback_test PRIVATE
    rts_ffmpeg_movie_playback
)
target_link_libraries(core_ffmpeg_video_player_compile PRIVATE
    corei_gameengine_include
    corei_libraries_include
    core_utility_no_pch
    rts_ffmpeg_movie_playback
    rts_xaudio2_audio_service
)

set_target_properties(core_ffmpeg_movie_playback_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_precompile_headers(core_ffmpeg_movie_playback_test PRIVATE <type_traits>)
target_precompile_headers(core_ffmpeg_video_player_compile PRIVATE <type_traits>)
add_dependencies(core_ffmpeg_movie_playback_test core_ffmpeg_video_player_compile)

add_test(NAME core_ffmpeg_movie_playback_test
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/RunFFmpegMoviePlaybackTest.ps1"
        -FFmpegExecutable "${FFMPEG_EXECUTABLE}"
        -TestExecutable "$<TARGET_FILE:core_ffmpeg_movie_playback_test>"
        -AudioFixturePath "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-movie-audio-video.mp4"
        -VideoFixturePath "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-movie-video-only.mp4"
        -RuntimeDirectory "${FFMPEG_RUNTIME_DIR}"
)
set_tests_properties(core_ffmpeg_movie_playback_test PROPERTIES TIMEOUT 60)
