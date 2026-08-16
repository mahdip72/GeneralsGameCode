add_executable(radar_overlay_prepare_tests)

target_sources(radar_overlay_prepare_tests PRIVATE
    RadarOverlayPrepareTest.cpp
    ../../GameEngineDevice/Source/W3DDevice/Common/System/RadarOverlayPrepare.cpp
    ../../GameEngineDevice/Source/W3DDevice/Common/System/RadarTerrainPrepare.cpp
)

target_link_libraries(radar_overlay_prepare_tests PRIVATE
    core_task_runtime
)

target_include_directories(radar_overlay_prepare_tests PRIVATE
    ../../Libraries/Include
    ../../GameEngineDevice/Include
)

set_target_properties(radar_overlay_prepare_tests PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

if(RTS_BUILD_CORE_EXTRAS)
    target_compile_definitions(radar_overlay_prepare_tests PRIVATE
        RTS_BUILD_CORE_EXTRAS
    )
endif()

if(MINGW)
    target_link_options(radar_overlay_prepare_tests PRIVATE -mconsole)
endif()

add_test(NAME radar_overlay_prepare_tests COMMAND radar_overlay_prepare_tests)
set_tests_properties(radar_overlay_prepare_tests PROPERTIES TIMEOUT 30)
