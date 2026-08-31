add_executable(projected_terrain_grid_tests
    ProjectedTerrainGridTest.cpp
    ../../GameEngineDevice/Source/W3DDevice/Common/System/RadarTerrainPrepare.cpp
)

target_include_directories(projected_terrain_grid_tests PRIVATE
    ../../Libraries/Include
    ../../GameEngineDevice/Include
)

target_link_libraries(projected_terrain_grid_tests PRIVATE
    core_task_runtime
)

if(RTS_BUILD_CORE_EXTRAS)
    target_compile_definitions(projected_terrain_grid_tests PRIVATE
        RTS_BUILD_CORE_EXTRAS
    )
endif()

set_target_properties(projected_terrain_grid_tests PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

if(MINGW)
    target_link_options(projected_terrain_grid_tests PRIVATE -mconsole)
endif()

add_test(NAME projected_terrain_grid_tests COMMAND projected_terrain_grid_tests)
set_tests_properties(projected_terrain_grid_tests PROPERTIES TIMEOUT 60)
add_test(NAME projected_terrain_grid_serial_tests
    COMMAND projected_terrain_grid_tests --serial)
set_tests_properties(projected_terrain_grid_serial_tests PROPERTIES TIMEOUT 60)
