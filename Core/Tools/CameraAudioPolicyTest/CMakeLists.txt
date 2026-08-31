add_executable(core_camera_audio_policy_tests)

target_sources(core_camera_audio_policy_tests PRIVATE
    CameraAudioPolicyTest.cpp
    ../../GameEngineDevice/Source/AudioDevice/AudioChannelPolicy.cpp
    ../../GameEngineDevice/Source/W3DDevice/GameClient/TerrainDrawSizing.cpp
)

target_include_directories(core_camera_audio_policy_tests PRIVATE
    ../../GameEngineDevice/Include
)

set_target_properties(core_camera_audio_policy_tests PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

if(MINGW)
    target_link_options(core_camera_audio_policy_tests PRIVATE -mconsole)
endif()

add_test(NAME core_camera_audio_policy_tests COMMAND core_camera_audio_policy_tests)
set_tests_properties(core_camera_audio_policy_tests PROPERTIES TIMEOUT 30)
