add_library(core_task_runtime STATIC)

target_sources(core_task_runtime PRIVATE
    TaskRuntime.cpp
    RadarTerrainKernel.cpp
    RadarOverlayKernel.cpp
    HeightMapTerrainKernel.cpp
    HeightMapDynamicLightKernel.cpp
)

target_include_directories(core_task_runtime PUBLIC
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
)

target_link_libraries(core_task_runtime PRIVATE
    $<LINK_ONLY:core_config>
)

target_link_libraries(core_task_runtime PUBLIC
    core_utility_no_pch
)

set_target_properties(core_task_runtime PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

if(RTS_BUILD_CORE_EXTRAS)
    target_compile_definitions(core_task_runtime PRIVATE
        RTS_BUILD_CORE_EXTRAS
    )
endif()

if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(core_task_runtime PRIVATE
        Threads::Threads
    )
endif()
