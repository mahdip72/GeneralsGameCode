add_library(core_task_runtime STATIC)

target_sources(core_task_runtime PRIVATE
    TaskRuntime.cpp
    PipelineExecutionPolicy.cpp
    SimulationExecutionPolicy.cpp
    SimulationCommandBuffer.cpp
    SimulationReadView.cpp
    ObjectComputationIsland.cpp
    SimulationPhaseGraph.cpp
    SimulationPhaseGraphOwnerAdapter.cpp
    DeterministicCrcStream.cpp
    DeterministicCrcRuntimeAdapter.cpp
    DeterministicCrcSnapshot.cpp
    DeterministicCrcJobSystemAdapter.cpp
    PhysicsIntegrationKernel.cpp
    ObjectStatusTimerKernel.cpp
    ImmutableSpatialQuery.cpp
    RadarTerrainKernel.cpp
    RadarOverlayKernel.cpp
    HeightMapTerrainKernel.cpp
    HeightMapDynamicLightKernel.cpp
    ParallelVisibility.cpp
    ParallelSkinning.cpp
    ParticleRenderKernel.cpp
    WaterMeshKernel.cpp
    ProjectedTerrainGridKernel.cpp
    WaterPolygonKernel.cpp
    SortingTriangleKernel.cpp
    GeometryTriangleDecode.cpp
)

if(NOT IS_VS6_BUILD)
    target_sources(core_task_runtime PRIVATE
        ImmutableSpatialQueryRuntime.cpp
        CounterBasedRng.cpp
        DeterministicAIPlanning.cpp
        DeterministicPathSearch.cpp
        DeterministicPathBatch.cpp
        JobSystem.cpp
        ResourceIoPipeline.cpp
        ModelAssetBytes.cpp
    )
endif()

if(IS_VS6_BUILD)
    target_sources(core_task_runtime PRIVATE
        JobSystemLegacy.cpp
    )
    # VC6's native STL and the product's STLport emit incompatible std::vector
    # implementations. Keep the runtime and every consumer on the same ABI.
    target_link_libraries(core_task_runtime PUBLIC stlport)
endif()

target_include_directories(core_task_runtime PUBLIC
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
)

target_link_libraries(core_task_runtime PRIVATE
    $<LINK_ONLY:core_config>
)

target_link_libraries(core_task_runtime PUBLIC
    core_utility_no_pch
)

if(IS_VS6_BUILD)
    set_target_properties(core_task_runtime PROPERTIES
        CXX_STANDARD 98
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
else()
    set_target_properties(core_task_runtime PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
endif()

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
