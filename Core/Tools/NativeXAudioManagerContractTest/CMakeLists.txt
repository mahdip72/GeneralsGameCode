add_library(core_native_xaudio_manager_contract_compile OBJECT
    NativeXAudioManagerContractTest.cpp
    ${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Source/XAudio2AudioDevice/XAudio2AudioManager.cpp
)

target_include_directories(core_native_xaudio_manager_contract_compile PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Include
    ${CMAKE_SOURCE_DIR}/Core/GameEngine/Include
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas
    ${CMAKE_SOURCE_DIR}/Core/Main
    ${CMAKE_SOURCE_DIR}/Dependencies/Utility
)

target_compile_options(core_native_xaudio_manager_contract_compile PRIVATE
    /FI${CMAKE_SOURCE_DIR}/Dependencies/Utility/Utility/CppMacros.h
)

set_target_properties(core_native_xaudio_manager_contract_compile PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_executable(core_native_xaudio_manager_contract_test
    NativeXAudioManagerContractTestRunner.cpp
)

set_target_properties(core_native_xaudio_manager_contract_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_native_xaudio_manager_contract_test
    COMMAND core_native_xaudio_manager_contract_test
)
set_tests_properties(core_native_xaudio_manager_contract_test PROPERTIES TIMEOUT 30)
