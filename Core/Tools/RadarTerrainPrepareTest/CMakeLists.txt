add_executable(radar_terrain_prepare_tests)

target_sources(radar_terrain_prepare_tests PRIVATE
    RadarTerrainPrepareTest.cpp
)

target_link_libraries(radar_terrain_prepare_tests PRIVATE
    core_task_runtime
)

set_target_properties(radar_terrain_prepare_tests PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

if(RTS_BUILD_CORE_EXTRAS)
    target_compile_definitions(radar_terrain_prepare_tests PRIVATE
        RTS_BUILD_CORE_EXTRAS
    )
endif()

if(MINGW)
    target_link_options(radar_terrain_prepare_tests PRIVATE -mconsole)
endif()

add_test(NAME radar_terrain_prepare_tests COMMAND radar_terrain_prepare_tests)
set_tests_properties(radar_terrain_prepare_tests PROPERTIES TIMEOUT 30)
