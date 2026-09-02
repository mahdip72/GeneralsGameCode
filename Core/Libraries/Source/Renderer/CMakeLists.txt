add_library(core_renderer STATIC
    LegacyRenderState.cpp
    NativeW3DOwnerQueue.cpp
    NativeW3DRenderState.cpp
    NativeW3DRenderer.cpp
    NativeW3DResources.cpp
    RendererDevice.cpp
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    # The target-kind capability is the only native GameEngineDevice seam
    # published before the title command implementation handoff.  It is
    # neutral and does not pull the compatibility adapter into x64.
    target_sources(core_renderer PRIVATE
        RenderGameClientNativeCapability.cpp
    )
endif()

if(WIN32 AND NOT IS_VS6_BUILD)
    set(_rts_fxc_roots)
    foreach(_rts_fxc_root IN ITEMS
        "${CMAKE_WINDOWS_KITS_10_DIR}"
        "$ENV{WindowsSdkDir}"
        "$ENV{CMAKE_WINDOWS_KITS_10_DIR}")
        if(_rts_fxc_root)
            file(TO_CMAKE_PATH "${_rts_fxc_root}" _rts_fxc_root)
            string(REGEX REPLACE "/+$" "" _rts_fxc_root "${_rts_fxc_root}")
            list(APPEND _rts_fxc_roots "${_rts_fxc_root}")
        endif()
    endforeach()

    cmake_host_system_information(
        RESULT _rts_fxc_registry_root
        QUERY WINDOWS_REGISTRY
            "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots"
        VALUE KitsRoot10
        VIEW BOTH)
    if(_rts_fxc_registry_root AND
        NOT _rts_fxc_registry_root MATCHES "REGISTRY-NOTFOUND")
        file(TO_CMAKE_PATH "${_rts_fxc_registry_root}"
            _rts_fxc_registry_root)
        string(REGEX REPLACE "/+$" "" _rts_fxc_registry_root
            "${_rts_fxc_registry_root}")
        list(APPEND _rts_fxc_roots "${_rts_fxc_registry_root}")
    endif()
    list(REMOVE_DUPLICATES _rts_fxc_roots)

    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(_rts_fxc_arches x86 x64)
    else()
        set(_rts_fxc_arches x64 x86)
    endif()

    set(_rts_fxc_versions
        "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}"
        "$ENV{WindowsSDKVersion}")
    set(_rts_fxc_hints)
    foreach(_rts_fxc_root IN LISTS _rts_fxc_roots)
        foreach(_rts_fxc_version IN LISTS _rts_fxc_versions)
            string(REGEX REPLACE "[/\\\\]+$" "" _rts_fxc_version
                "${_rts_fxc_version}")
            if(_rts_fxc_version MATCHES "^10\\.0\\.[0-9]+\\.[0-9]+$")
                foreach(_rts_fxc_arch IN LISTS _rts_fxc_arches)
                    list(APPEND _rts_fxc_hints
                        "${_rts_fxc_root}/bin/${_rts_fxc_version}/${_rts_fxc_arch}")
                endforeach()
            endif()
        endforeach()
        foreach(_rts_fxc_arch IN LISTS _rts_fxc_arches)
            file(GLOB _rts_fxc_arch_dirs LIST_DIRECTORIES TRUE
                "${_rts_fxc_root}/bin/10.*/${_rts_fxc_arch}")
            list(SORT _rts_fxc_arch_dirs COMPARE NATURAL ORDER DESCENDING)
            list(APPEND _rts_fxc_hints ${_rts_fxc_arch_dirs})
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES _rts_fxc_hints)

    find_program(RTS_FXC_EXECUTABLE
        NAMES fxc.exe fxc
        HINTS ${_rts_fxc_hints}
        DOC "Path to the Windows SDK fxc shader compiler")
    if(NOT RTS_FXC_EXECUTABLE)
        message(FATAL_ERROR
            "The Windows SDK fxc shader compiler was not found. Install the "
            "Windows 10/11 SDK or set RTS_FXC_EXECUTABLE to fxc.exe.")
    endif()
    set(renderer_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/Generated")
    set(renderer_shader_source "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/LegacyFixedFunction.hlsl")
    set(renderer_vertex_shader_header "${renderer_generated_dir}/LegacyFixedFunctionVS.h")
    set(renderer_pixel_shader_header "${renderer_generated_dir}/LegacyFixedFunctionPS.h")
    set(renderer_water_flat_pixel_shader_header "${renderer_generated_dir}/LegacyWaterFlatPS.h")
    set(renderer_water_river_pixel_shader_header "${renderer_generated_dir}/LegacyWaterRiverPS.h")
    set(renderer_sea_wave_vertex_shader_header "${renderer_generated_dir}/LegacySeaWaveVS.h")
    set(renderer_sea_wave_pixel_shader_header "${renderer_generated_dir}/LegacySeaWavePS.h")
    set(renderer_textured_vertex_shader_header "${renderer_generated_dir}/LegacyTexturedVS.h")
    set(renderer_textured_unweighted_vertex_shader_header "${renderer_generated_dir}/LegacyTexturedUnweightedVS.h")
    set(renderer_textured_pixel_shader_header "${renderer_generated_dir}/LegacyTexturedPS.h")
    set(renderer_textured_fixed1_pixel_shader_header "${renderer_generated_dir}/LegacyTexturedFixed1PS.h")
    set(renderer_textured_fixed2_pixel_shader_header "${renderer_generated_dir}/LegacyTexturedFixed2PS.h")
    set(renderer_presentation_shader_source
        "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/PresentationColorTransform.hlsl")
    set(renderer_presentation_vertex_shader_header
        "${renderer_generated_dir}/PresentationColorTransformVS.h")
    set(renderer_presentation_pixel_shader_header
        "${renderer_generated_dir}/PresentationColorTransformPS.h")

    function(add_legacy_pixel_program_shader output_variable file_name symbol program)
        set(output_path "${renderer_generated_dir}/${file_name}")
        add_custom_command(
            OUTPUT "${output_path}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${renderer_generated_dir}"
            COMMAND "${RTS_FXC_EXECUTABLE}" /nologo /O3 /T ps_5_0 /E PSTextured
                /D LEGACY_PIXEL_PROGRAM=${program}
                /Fh "${output_path}"
                /Vn ${symbol} "${renderer_shader_source}"
            DEPENDS "${renderer_shader_source}"
            VERBATIM
        )
        set(${output_variable} "${output_path}" PARENT_SCOPE)
    endfunction()

    add_legacy_pixel_program_shader(renderer_terrain_base_pixel_shader_header
        LegacyTerrainBasePS.h g_LegacyTerrainBasePS 3)
    add_legacy_pixel_program_shader(renderer_terrain_noise_pixel_shader_header
        LegacyTerrainNoisePS.h g_LegacyTerrainNoisePS 4)
    add_legacy_pixel_program_shader(renderer_terrain_noise2_pixel_shader_header
        LegacyTerrainNoise2PS.h g_LegacyTerrainNoise2PS 5)
    add_legacy_pixel_program_shader(renderer_road_noise2_pixel_shader_header
        LegacyRoadNoise2PS.h g_LegacyRoadNoise2PS 6)
    add_legacy_pixel_program_shader(renderer_flat_terrain_base0_pixel_shader_header
        LegacyFlatTerrainBase0PS.h g_LegacyFlatTerrainBase0PS 7)
    add_legacy_pixel_program_shader(renderer_flat_terrain_base_pixel_shader_header
        LegacyFlatTerrainBasePS.h g_LegacyFlatTerrainBasePS 8)
    add_legacy_pixel_program_shader(renderer_flat_terrain_noise_pixel_shader_header
        LegacyFlatTerrainNoisePS.h g_LegacyFlatTerrainNoisePS 9)
    add_legacy_pixel_program_shader(renderer_flat_terrain_noise2_pixel_shader_header
        LegacyFlatTerrainNoise2PS.h g_LegacyFlatTerrainNoise2PS 10)

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
        OUTPUT "${renderer_textured_unweighted_vertex_shader_header}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${renderer_generated_dir}"
        COMMAND "${RTS_FXC_EXECUTABLE}" /nologo /T vs_5_0 /E VSTextured
            /D LEGACY_UNWEIGHTED_VERTEX_INPUT=1
            /Fh "${renderer_textured_unweighted_vertex_shader_header}"
            /Vn g_LegacyTexturedUnweightedVS "${renderer_shader_source}"
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
    add_custom_command(
        OUTPUT "${renderer_textured_fixed1_pixel_shader_header}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${renderer_generated_dir}"
        COMMAND "${RTS_FXC_EXECUTABLE}" /nologo /T ps_5_0 /E PSTextured
            /D LEGACY_FIXED_FUNCTION_ONLY=1
            /D LEGACY_ACTIVE_TEXTURE_STAGE_COUNT=1
            /Fh "${renderer_textured_fixed1_pixel_shader_header}"
            /Vn g_LegacyTexturedFixed1PS "${renderer_shader_source}"
        DEPENDS "${renderer_shader_source}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${renderer_textured_fixed2_pixel_shader_header}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${renderer_generated_dir}"
        COMMAND "${RTS_FXC_EXECUTABLE}" /nologo /T ps_5_0 /E PSTextured
            /D LEGACY_FIXED_FUNCTION_ONLY=1
            /D LEGACY_ACTIVE_TEXTURE_STAGE_COUNT=2
            /Fh "${renderer_textured_fixed2_pixel_shader_header}"
            /Vn g_LegacyTexturedFixed2PS "${renderer_shader_source}"
        DEPENDS "${renderer_shader_source}"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${renderer_presentation_vertex_shader_header}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${renderer_generated_dir}"
        COMMAND "${RTS_FXC_EXECUTABLE}" /nologo /T vs_5_0 /E VSMain
            /Fh "${renderer_presentation_vertex_shader_header}"
            /Vn g_PresentationColorTransformVS
            "${renderer_presentation_shader_source}"
        DEPENDS "${renderer_presentation_shader_source}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${renderer_presentation_pixel_shader_header}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${renderer_generated_dir}"
        COMMAND "${RTS_FXC_EXECUTABLE}" /nologo /T ps_5_0 /E PSMain
            /Fh "${renderer_presentation_pixel_shader_header}"
            /Vn g_PresentationColorTransformPS
            "${renderer_presentation_shader_source}"
        DEPENDS "${renderer_presentation_shader_source}"
        VERBATIM
    )

    target_sources(core_renderer PRIVATE
        D3D11RenderDevice.cpp
        ThreadedRenderDevice.cpp
        "${renderer_vertex_shader_header}"
        "${renderer_pixel_shader_header}"
        "${renderer_water_flat_pixel_shader_header}"
        "${renderer_water_river_pixel_shader_header}"
        "${renderer_sea_wave_vertex_shader_header}"
        "${renderer_sea_wave_pixel_shader_header}"
        "${renderer_textured_vertex_shader_header}"
        "${renderer_textured_unweighted_vertex_shader_header}"
        "${renderer_textured_pixel_shader_header}"
        "${renderer_textured_fixed1_pixel_shader_header}"
        "${renderer_textured_fixed2_pixel_shader_header}"
        "${renderer_terrain_base_pixel_shader_header}"
        "${renderer_terrain_noise_pixel_shader_header}"
        "${renderer_terrain_noise2_pixel_shader_header}"
        "${renderer_road_noise2_pixel_shader_header}"
        "${renderer_flat_terrain_base0_pixel_shader_header}"
        "${renderer_flat_terrain_base_pixel_shader_header}"
        "${renderer_flat_terrain_noise_pixel_shader_header}"
        "${renderer_flat_terrain_noise2_pixel_shader_header}"
        "${renderer_presentation_vertex_shader_header}"
        "${renderer_presentation_pixel_shader_header}"
    )
    target_include_directories(core_renderer PRIVATE
        "${renderer_generated_dir}"
    )
    target_compile_definitions(core_renderer PUBLIC
        RTS_RENDERER_HAS_D3D11=1
    )
    target_link_libraries(core_renderer PRIVATE
        core_task_runtime
        d3d11
        dxgi
        user32
    )
endif()

target_include_directories(core_renderer PUBLIC
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas
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
