add_executable(height_map_terrain_prepare_tests)

target_sources(height_map_terrain_prepare_tests PRIVATE
    HeightMapTerrainPrepareTest.cpp
    ../../GameEngineDevice/Source/W3DDevice/Common/System/RadarTerrainPrepare.cpp
    ../../GameEngineDevice/Source/W3DDevice/Common/System/HeightMapTerrainPrepare.cpp
)

target_include_directories(height_map_terrain_prepare_tests PRIVATE
    ../../Libraries/Include
    ../../GameEngineDevice/Include
)

target_link_libraries(height_map_terrain_prepare_tests PRIVATE
    core_task_runtime
)

set_target_properties(height_map_terrain_prepare_tests PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

if(RTS_BUILD_CORE_EXTRAS)
    target_compile_definitions(height_map_terrain_prepare_tests PRIVATE
        RTS_BUILD_CORE_EXTRAS
    )
endif()

if(MINGW)
    target_link_options(height_map_terrain_prepare_tests PRIVATE -mconsole)
endif()

add_test(NAME height_map_terrain_prepare_tests COMMAND height_map_terrain_prepare_tests)
set_tests_properties(height_map_terrain_prepare_tests PROPERTIES TIMEOUT 30)
