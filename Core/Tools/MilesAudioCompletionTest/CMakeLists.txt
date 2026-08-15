add_executable(core_miles_audio_completion_tests
    MilesAudioCompletionTest.cpp
    ../../GameEngineDevice/Include/MilesAudioDevice/MilesAudioCompletionQueue.h
)

target_include_directories(core_miles_audio_completion_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../GameEngineDevice/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Libraries/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../GameEngine/Include
    ${PROJECT_SOURCE_DIR}/Dependencies/Utility
)

set_target_properties(core_miles_audio_completion_tests PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(core_miles_audio_completion_tests PRIVATE /W4)
endif()

if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(core_miles_audio_completion_tests PRIVATE Threads::Threads)
endif()

include(CTest)
if(BUILD_TESTING)
    add_test(NAME core_miles_audio_completion_tests COMMAND core_miles_audio_completion_tests)
    set_tests_properties(core_miles_audio_completion_tests PROPERTIES TIMEOUT 30)
endif()
