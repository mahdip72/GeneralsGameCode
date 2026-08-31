add_executable(core_xaudio2_pcm_voice_test
    XAudio2PcmVoiceTest.cpp
)

target_include_directories(core_xaudio2_pcm_voice_test PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Include
)

target_link_libraries(core_xaudio2_pcm_voice_test PRIVATE
    rts_xaudio2_pcm_voice
)

set_target_properties(core_xaudio2_pcm_voice_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_xaudio2_pcm_voice_test COMMAND core_xaudio2_pcm_voice_test)
set_tests_properties(core_xaudio2_pcm_voice_test PROPERTIES TIMEOUT 30)
