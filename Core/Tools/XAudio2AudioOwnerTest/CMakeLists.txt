add_executable(core_xaudio2_audio_owner_test XAudio2AudioOwnerTest.cpp)
target_include_directories(core_xaudio2_audio_owner_test PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Include
)
target_link_libraries(core_xaudio2_audio_owner_test PRIVATE rts_xaudio2_audio_service)
set_target_properties(core_xaudio2_audio_owner_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_xaudio2_audio_owner_test COMMAND core_xaudio2_audio_owner_test)
set_tests_properties(core_xaudio2_audio_owner_test PROPERTIES TIMEOUT 30)
