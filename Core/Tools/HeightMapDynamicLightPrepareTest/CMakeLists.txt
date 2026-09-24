add_executable(height_map_dynamic_light_prepare_tests)

target_sources(height_map_dynamic_light_prepare_tests PRIVATE
    HeightMapDynamicLightPrepareTest.cpp
    ../../GameEngineDevice/Source/W3DDevice/Common/System/RadarTerrainPrepare.cpp
    ../../GameEngineDevice/Source/W3DDevice/Common/System/HeightMapDynamicLightPrepare.cpp
)

target_include_directories(height_map_dynamic_light_prepare_tests PRIVATE
    ../../Libraries/Include
    ../../GameEngineDevice/Include
)

target_link_libraries(height_map_dynamic_light_prepare_tests PRIVATE
    core_task_runtime
)

set_target_properties(height_map_dynamic_light_prepare_tests PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

if(RTS_BUILD_CORE_EXTRAS)
    target_compile_definitions(height_map_dynamic_light_prepare_tests PRIVATE
        RTS_BUILD_CORE_EXTRAS
    )
endif()

if(MINGW)
    target_link_options(height_map_dynamic_light_prepare_tests PRIVATE -mconsole)
endif()

add_test(NAME height_map_dynamic_light_prepare_tests
    COMMAND height_map_dynamic_light_prepare_tests)
set_tests_properties(height_map_dynamic_light_prepare_tests PROPERTIES TIMEOUT 30)

add_executable(height_map_dynamic_light_envelope_tests
    HeightMapDynamicLightEnvelopeTest.cpp)
target_include_directories(height_map_dynamic_light_envelope_tests PRIVATE
    ../../Libraries/Include)
target_include_directories(height_map_dynamic_light_envelope_tests PRIVATE
    ../../../Dependencies/Utility)
set_target_properties(height_map_dynamic_light_envelope_tests PROPERTIES
    CXX_STANDARD 98 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
add_test(NAME height_map_dynamic_light_envelope_tests
    COMMAND height_map_dynamic_light_envelope_tests)
set_tests_properties(height_map_dynamic_light_envelope_tests
    PROPERTIES TIMEOUT 30)

if(WIN32 AND MSVC)
    add_executable(height_map_dynamic_light_far_cull_parity_tests
        HeightMapDynamicLightFarCullParityTest.cpp
        HeightMapDynamicLightParityBaseline.cpp)
    target_include_directories(height_map_dynamic_light_far_cull_parity_tests PRIVATE
        ../../Libraries/Include)
    target_link_libraries(height_map_dynamic_light_far_cull_parity_tests PRIVATE
        core_task_runtime)
    set_target_properties(height_map_dynamic_light_far_cull_parity_tests PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    add_test(NAME height_map_dynamic_light_far_cull_parity_tests
        COMMAND height_map_dynamic_light_far_cull_parity_tests)
    set_tests_properties(height_map_dynamic_light_far_cull_parity_tests
        PROPERTIES TIMEOUT 60)
endif()

if(WIN32)
    add_test(NAME height_map_buffer_failure_contract_tests
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/HeightMapBufferFailureContract.Tests.ps1"
            -SourceRoot "${CMAKE_SOURCE_DIR}")
    set_tests_properties(height_map_buffer_failure_contract_tests PROPERTIES TIMEOUT 30)
endif()
