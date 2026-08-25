add_library(core_renderer STATIC
    LegacyRenderState.cpp
    NativeW3DOwnerQueue.cpp
    NativeW3DRenderState.cpp
    NativeW3DRenderer.cpp
    NativeW3DResources.cpp
    RendererDevice.cpp
)

if(WIN32 AND NOT IS_VS6_BUILD)
    include(windows-sdk-tools)
    rts_find_windows_sdk_program(RTS_FXC_EXECUTABLE NAMES fxc.exe fxc)
    set(renderer_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/Generated")
    set(renderer_shader_source "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/LegacyFixedFunction.hlsl")
    set(renderer_vertex_shader_header "${renderer_generated_dir}/LegacyFixedFunctionVS.h")
    set(renderer_pixel_shader_header "${renderer_generated_dir}/LegacyFixedFunctionPS.h")
    set(renderer_water_flat_pixel_shader_header "${renderer_generated_dir}/LegacyWaterFlatPS.h")
    set(renderer_water_river_pixel_shader_header "${renderer_generated_dir}/LegacyWaterRiverPS.h")
    set(renderer_sea_wave_vertex_shader_header "${renderer_generated_dir}/LegacySeaWaveVS.h")
    set(renderer_sea_wave_pixel_shader_header "${renderer_generated_dir}/LegacySeaWavePS.h")
    set(renderer_textured_vertex_shader_header "${renderer_generated_dir}/LegacyTexturedVS.h")
    set(renderer_textured_pixel_shader_header "${renderer_generated_dir}/LegacyTexturedPS.h")

    add_custom_command(
        OUTPUT "${renderer_vertex_shader_header}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${renderer_generated_dir}"
        COMMAND "${RTS_FXC_EXECUTABLE}" /nologo /T vs_5_0 /E VSMain
            /Fh "${renderer_vertex_shader_header}"
            /Vn g_LegacyFixedFunctionVS "${renderer_shader_source}"
        DEPENDS "${renderer_shader_source}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${renderer_pixel_shader_header}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${renderer_generated_dir}"
        COMMAND "${RTS_FXC_EXECUTABLE}" /nologo /T ps_5_0 /E PSMain
            /Fh "${renderer_pixel_shader_header}"
            /Vn g_LegacyFixedFunctionPS "${renderer_shader_source}"
        DEPENDS "${renderer_shader_source}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${renderer_water_flat_pixel_shader_header}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${renderer_generated_dir}"
        COMMAND "${RTS_FXC_EXECUTABLE}" /nologo /T ps_5_0 /E PSWaterFlat
            /Fh "${renderer_water_flat_pixel_shader_header}"
            /Vn g_LegacyWaterFlatPS "${renderer_shader_source}"
        DEPENDS "${renderer_shader_source}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${renderer_water_river_pixel_shader_header}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${renderer_generated_dir}"
        COMMAND "${RTS_FXC_EXECUTABLE}" /nologo /T ps_5_0 /E PSWaterRiver
            /Fh "${renderer_water_river_pixel_shader_header}"
            /Vn g_LegacyWaterRiverPS "${renderer_shader_source}"
        DEPENDS "${renderer_shader_source}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${renderer_sea_wave_vertex_shader_header}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${renderer_generated_dir}"
        COMMAND "${RTS_FXC_EXECUTABLE}" /nologo /T vs_5_0 /E VSSeaWave
            /Fh "${renderer_sea_wave_vertex_shader_header}"
            /Vn g_LegacySeaWaveVS "${renderer_shader_source}"
        DEPENDS "${renderer_shader_source}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${renderer_sea_wave_pixel_shader_header}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${renderer_generated_dir}"
        COMMAND "${RTS_FXC_EXECUTABLE}" /nologo /T ps_5_0 /E PSSeaWave
            /Fh "${renderer_sea_wave_pixel_shader_header}"
            /Vn g_LegacySeaWavePS "${renderer_shader_source}"
        DEPENDS "${renderer_shader_source}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${renderer_textured_vertex_shader_header}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${renderer_generated_dir}"
        COMMAND "${RTS_FXC_EXECUTABLE}" /nologo /T vs_5_0 /E VSTextured
            /Fh "${renderer_textured_vertex_shader_header}"
            /Vn g_LegacyTexturedVS "${renderer_shader_source}"
        DEPENDS "${renderer_shader_source}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${renderer_textured_pixel_shader_header}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${renderer_generated_dir}"
        COMMAND "${RTS_FXC_EXECUTABLE}" /nologo /T ps_5_0 /E PSTextured
            /Fh "${renderer_textured_pixel_shader_header}"
            /Vn g_LegacyTexturedPS "${renderer_shader_source}"
        DEPENDS "${renderer_shader_source}"
        VERBATIM
    )

    target_sources(core_renderer PRIVATE
        D3D11RenderDevice.cpp
        "${renderer_vertex_shader_header}"
        "${renderer_pixel_shader_header}"
        "${renderer_water_flat_pixel_shader_header}"
        "${renderer_water_river_pixel_shader_header}"
        "${renderer_sea_wave_vertex_shader_header}"
        "${renderer_sea_wave_pixel_shader_header}"
        "${renderer_textured_vertex_shader_header}"
        "${renderer_textured_pixel_shader_header}"
    )
    target_include_directories(core_renderer PRIVATE
        "${renderer_generated_dir}"
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

target_link_libraries(core_renderer PRIVATE
    core_utility_no_pch
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
