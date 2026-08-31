add_executable(effect_prepare_tests
    EffectPrepareTest.cpp
    ../../GameEngineDevice/Source/W3DDevice/Common/System/EffectPrepare.cpp
    ../../GameEngineDevice/Source/W3DDevice/Common/System/RadarTerrainPrepare.cpp
)
target_include_directories(effect_prepare_tests PRIVATE
    ../../Libraries/Include
    ../../GameEngineDevice/Include
)
target_link_libraries(effect_prepare_tests PRIVATE core_task_runtime)
if(RTS_BUILD_CORE_EXTRAS)
    target_compile_definitions(effect_prepare_tests PRIVATE RTS_BUILD_CORE_EXTRAS)
endif()
set_target_properties(effect_prepare_tests PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
if(MINGW)
    target_link_options(effect_prepare_tests PRIVATE -mconsole)
endif()

add_test(NAME effect_prepare_tests COMMAND effect_prepare_tests)
set_tests_properties(effect_prepare_tests PROPERTIES TIMEOUT 60)
add_test(NAME effect_prepare_serial_tests COMMAND effect_prepare_tests --serial)
set_tests_properties(effect_prepare_serial_tests PROPERTIES TIMEOUT 60)
