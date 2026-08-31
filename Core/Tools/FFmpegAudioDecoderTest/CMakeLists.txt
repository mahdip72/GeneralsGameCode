find_package(FFMPEG REQUIRED)

add_executable(core_ffmpeg_audio_decoder_test
    FFmpegAudioDecoderTest.cpp
)

target_link_libraries(core_ffmpeg_audio_decoder_test PRIVATE
    rts_ffmpeg_audio_decoder
)

set_target_properties(core_ffmpeg_audio_decoder_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_ffmpeg_audio_decoder_test COMMAND core_ffmpeg_audio_decoder_test)
set_tests_properties(core_ffmpeg_audio_decoder_test PROPERTIES TIMEOUT 30)
if(WIN32 AND FFMPEG_RUNTIME_DIR)
    set_tests_properties(core_ffmpeg_audio_decoder_test PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${FFMPEG_RUNTIME_DIR}"
    )
endif()
