add_library(core_native_audio_manager_contract_compile OBJECT
    NativeAudioManagerContractTest.cpp
    ${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Source/AudioDevice/NullAudioManager.cpp
    ${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Source/XAudio2AudioDevice/AudioManagerFactory.cpp
    ${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Source/XAudio2AudioDevice/XAudio2AudioManager.cpp
)

target_include_directories(core_native_audio_manager_contract_compile PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Include
    ${CMAKE_SOURCE_DIR}/Core/GameEngine/Include
    ${CMAKE_SOURCE_DIR}/Core/GameEngine/Include/Precompiled
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas
    ${CMAKE_SOURCE_DIR}/Core/Main
    ${CMAKE_SOURCE_DIR}/Dependencies/Utility
)

target_compile_options(core_native_audio_manager_contract_compile PRIVATE
    /FI${CMAKE_SOURCE_DIR}/Dependencies/Utility/Utility/CppMacros.h
)

add_executable(core_native_audio_manager_contract_test
    NativeAudioManagerContractTestRunner.cpp
)

set_target_properties(core_native_audio_manager_contract_compile PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

set_target_properties(core_native_audio_manager_contract_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_native_audio_manager_contract_test
    COMMAND core_native_audio_manager_contract_test
)
set_tests_properties(core_native_audio_manager_contract_test PROPERTIES TIMEOUT 30)
