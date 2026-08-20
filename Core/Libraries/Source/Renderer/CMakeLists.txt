add_library(core_renderer STATIC
    LegacyRenderState.cpp
    RendererDevice.cpp
)

if(WIN32 AND NOT IS_VS6_BUILD)
    target_sources(core_renderer PRIVATE
        D3D11RenderDevice.cpp
    )
    target_compile_definitions(core_renderer PUBLIC
        RTS_RENDERER_HAS_D3D11=1
    )
    target_link_libraries(core_renderer PRIVATE
        d3d11
        dxgi
    )
endif()

target_include_directories(core_renderer PUBLIC
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
)

if(IS_VS6_BUILD)
    set_target_properties(core_renderer PROPERTIES
        CXX_STANDARD 98
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
else()
    set_target_properties(core_renderer PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
endif()
