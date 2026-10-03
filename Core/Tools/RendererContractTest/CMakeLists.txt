add_executable(core_renderer_contract_tests
    RendererContractTest.cpp
)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_sources(core_renderer_contract_tests PRIVATE
        LegacyResetResourceTest.cpp
        LegacyAsyncFramePolicyTest.cpp
        LegacyAsyncBridgeCompletionTest.cpp
        LegacyWaveShaderContractTest.cpp
        LegacyShaderAssetContractTest.cpp)

    # Compile the actual reset/publication methods against device-free doubles
    # so failed HRESULTs and retained COM references are exercised, not
    # source-matched.  These fragments are deliberately an x86-only fixture;
    # the native x64 contract tests must not parse compatibility sources.
    function(renderer_reset_source_fragment output source start_marker end_marker)
        set(source "${CMAKE_SOURCE_DIR}/${source}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
        file(READ "${source}" contents)
        string(REPLACE "\r\n" "\n" contents "${contents}")
        string(FIND "${contents}" "${start_marker}" start)
        if(start LESS 0)
            message(FATAL_ERROR "Renderer reset test start marker missing in ${source}")
        endif()
        string(SUBSTRING "${contents}" ${start} -1 fragment)
        string(FIND "${fragment}" "${end_marker}" end)
        if(end LESS 0)
            message(FATAL_ERROR "Renderer reset test end marker missing in ${source}")
        endif()
        string(SUBSTRING "${fragment}" 0 ${end} fragment)
        set(${output} "${fragment}" PARENT_SCOPE)
    endfunction()

    renderer_reset_source_fragment(async_service_methods
        Core/LegacyRenderer/WWVegas/WW3D2/d3d11legacybridge.cpp
        "\tvoid Service_Render_Completions()" "\n\tRenderResult End_Threaded_Frame(")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/LegacyAsyncServiceMethods.inc"
        "${async_service_methods}\n")

    renderer_reset_source_fragment(reset_methods
        Core/LegacyRenderer/WWVegas/WW3D2/dx8wrapper.cpp
        "bool DX8Wrapper::Reset_Device(" "void DX8Wrapper::Release_Device()")
    renderer_reset_source_fragment(buffer_methods
        Core/LegacyRenderer/WWVegas/WW3D2/dx8wrapper.cpp
        "void DX8Wrapper::Track_DX8_Vertex_Buffer(" "HRESULT DX8Wrapper::Draw_DX8_Indexed_Primitive(")
    renderer_reset_source_fragment(texture_release
        Core/LegacyRenderer/WWVegas/WW3D2/texture_legacy.cpp
        "void TextureBaseClass::Release_D3D_Texture()" "void TextureBaseClass::Invalidate_Old_Unused_Textures(")
    renderer_reset_source_fragment(texture_set
        Core/LegacyRenderer/WWVegas/WW3D2/texture_legacy.cpp
        "void TextureBaseClass::Set_D3D_Base_Texture(" "void TextureBaseClass::Load_Locked_Surface()")
    renderer_reset_source_fragment(texture_apply
        Core/LegacyRenderer/WWVegas/WW3D2/texture_legacy.cpp
        "void TextureClass::Apply_New_Surface\n" "void TextureClass::Apply(unsigned int stage)")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/LegacyResetMethods.inc"
        "${reset_methods}\n${buffer_methods}\n${texture_release}\n${texture_set}\n${texture_apply}\n")

    # Exercise the real scene acceptance and HRESULT mapping, including queued
    # native frames, without starting a game or requiring a graphics device.
    renderer_reset_source_fragment(frame_result_mapping
        Core/LegacyRenderer/WWVegas/WW3D2/dx8wrapper.cpp
        "HRESULT Render_Result_To_HRESULT(" "bool Record_Unavailable_Visible_Submission(")
    renderer_reset_source_fragment(end_scene_method
        Core/LegacyRenderer/WWVegas/WW3D2/dx8wrapper.cpp
        "void DX8Wrapper::End_Scene(bool flip_frames)" "void DX8Wrapper::Flip_To_Primary()")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/LegacyAsyncFrameMethods.inc"
        "${frame_result_mapping}\n${end_scene_method}\n")

    # Exercise completion-time target retention and cache rebinding using the
    # production bridge methods, independently of a real graphics device.
    renderer_reset_source_fragment(async_target_methods
        Core/LegacyRenderer/WWVegas/WW3D2/d3d11legacybridge.cpp
        "\tIDirect3DSurface8 *Retain_Target_Surface(" "\n\tvoid Service_Render_Completions()")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/LegacyAsyncTargetMethods.inc"
        "${async_target_methods}\n")

    # Exercise the unchanged wave shader call block against a recording
    # creator.  This keeps the asset/declaration boundary behavioral rather
    # than reducing the regression to a source-text search.
    renderer_reset_source_fragment(wave_shader_calls
        Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp
        "\t\t// The shader seam"
        "\n\t\tREF_PTR_RELEASE(m_pReflectionTexture);")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/LegacyWaveShaderCalls.inc"
        "${wave_shader_calls}\n")

    # Run both title loaders with controlled provider/device failures. These
    # are the production definitions, not a reimplementation of the loader.
    renderer_reset_source_fragment(generals_shader_asset_method
        Core/LegacyRenderer/Generals/WW3D2/RenderGameClientLegacy.cpp
        "HRESULT CreateLegacyD3DShader(" "RenderResult SetGameShaderCullInverted(")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/GeneralsLegacyShaderAssetMethods.inc"
        "${generals_shader_asset_method}\n")
    renderer_reset_source_fragment(zerohour_shader_asset_method
        Core/LegacyRenderer/GeneralsMD/WW3D2/RenderGameClientLegacy.cpp
        "HRESULT CreateLegacyD3DShader(" "RenderResult SetGameShaderCullInverted(")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ZeroHourLegacyShaderAssetMethods.inc"
        "${zerohour_shader_asset_method}\n")
endif()

target_link_libraries(core_renderer_contract_tests PRIVATE core_renderer)
target_compile_definitions(core_renderer_contract_tests PRIVATE
    RTS_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_link_libraries(core_renderer_contract_tests PRIVATE d3d8lib)
    target_compile_definitions(core_renderer_contract_tests PRIVATE
        RTS_RENDERER_RESET_TEST_LEGACY_FRAGMENT=1
        RTS_RENDERER_WAVE_SHADER_CONTRACT_TEST=1
        RTS_RENDERER_SHADER_ASSET_CONTRACT_TEST=1)
    target_include_directories(core_renderer_contract_tests BEFORE PRIVATE
        ${CMAKE_SOURCE_DIR}/Core/LegacyRenderer/WWVegas
        ${CMAKE_SOURCE_DIR}/Core/LegacyRenderer/WWVegas/WW3D2
        ${CMAKE_SOURCE_DIR}/Core/LegacyRenderer/GameEngineDevice/Include)
else()
    target_compile_definitions(core_renderer_contract_tests PRIVATE
        RTS_RENDERER_NATIVE_CONTRACT_ONLY=1)
endif()
if(TARGET core_ww3d2_native)
    target_link_libraries(core_renderer_contract_tests PRIVATE
        core_ww3d2_native
    )
endif()

target_include_directories(core_renderer_contract_tests PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/Core/GameEngine/Include
    ${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Include
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas
    ${CMAKE_SOURCE_DIR}/Dependencies/Utility
)

if(IS_VS6_BUILD)
    target_link_libraries(core_renderer_contract_tests PRIVATE stlport)
    set_target_properties(core_renderer_contract_tests PROPERTIES
        CXX_STANDARD 98
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
else()
    set_target_properties(core_renderer_contract_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
endif()

add_test(NAME core_renderer_contract_tests
    COMMAND core_renderer_contract_tests
)
set_tests_properties(core_renderer_contract_tests PROPERTIES TIMEOUT 30)

if(WIN32 AND NOT IS_VS6_BUILD)

    # Execute the production sampler lifetime methods and their real gather/
    # bind callsites against a recording device. Missing or duplicate markers
    # fail configuration; the generated proof records exact normalized bodies.
    set(d3d11_sampler_source
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/Renderer/D3D11RenderDevice.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${d3d11_sampler_source}")
    file(READ "${d3d11_sampler_source}" d3d11_sampler_contents)
    string(REPLACE "\r\n" "\n" d3d11_sampler_contents "${d3d11_sampler_contents}")
    file(SHA256 "${d3d11_sampler_source}" d3d11_sampler_source_hash)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/D3D11SamplerSourceProof.txt"
        "source=${d3d11_sampler_source}\nsource_sha256=${d3d11_sampler_source_hash}\n")
    function(d3d11_sampler_fragment name start_marker end_marker)
        string(FIND "${d3d11_sampler_contents}" "${start_marker}" start)
        if(start LESS 0)
            message(FATAL_ERROR "D3D11 sampler fixture start marker missing: ${name}")
        endif()
        string(SUBSTRING "${d3d11_sampler_contents}" ${start} -1 fragment)
        string(LENGTH "${start_marker}" marker_length)
        string(SUBSTRING "${fragment}" ${marker_length} -1 remainder)
        string(FIND "${remainder}" "${start_marker}" duplicate)
        if(NOT duplicate LESS 0)
            message(FATAL_ERROR "D3D11 sampler fixture marker duplicated: ${name}")
        endif()
        string(FIND "${fragment}" "${end_marker}" end)
        if(end LESS 0)
            message(FATAL_ERROR "D3D11 sampler fixture end marker missing: ${name}")
        endif()
        string(SUBSTRING "${fragment}" 0 ${end} fragment)
        string(SHA256 fragment_hash "${fragment}")
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${name}.inc"
            "// Production source body SHA256: ${fragment_hash}\n${fragment}\n")
        file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/D3D11SamplerSourceProof.txt"
            "${name}_sha256=${fragment_hash}\n")
    endfunction()
    d3d11_sampler_fragment(D3D11SamplerConstants
        "const unsigned int STATE_CACHE_CAPACITY ="
        "\nconst unsigned int TRANSFORM_CONSTANT_BUFFER_COUNT")
    d3d11_sampler_fragment(D3D11SamplerEntry
        "struct SamplerStateEntry" "\nstruct InputLayoutEntry")
    d3d11_sampler_fragment(D3D11SamplerSerial
        "\tunsigned int nextStateUseSerial()" "\n\tbool evictBlendState()")
    d3d11_sampler_fragment(D3D11SamplerEviction
        "\tbool evictSamplerState(" "\n\tbool evictInputLayout()")
    d3d11_sampler_fragment(D3D11SamplerLookup
        "\tHRESULT findOrCreateSamplerState(" "\n\tHRESULT findOrCreateInputLayout(")
    d3d11_sampler_fragment(D3D11SamplerGatherCall
        "\t\t\t\tresult = findOrCreateSamplerState(samplerDescriptor,"
        "\n\t\t\t}\n\t\t}")
    d3d11_sampler_fragment(D3D11SamplerBindCall
        "\t\tif (hasTextures && (forcePipeline || !m_pipelineHasTextures ||"
        "\n\t\tm_boundBlendDescriptor =")

    # Supplement real-backend pixel/lifecycle coverage with the exact buffer
    # update method against a recording Map double, exposing private mirror
    # bytes and failed-map boundaries without a new production debug API.
    d3d11_sampler_fragment(D3D11BufferRangeMethods
        "enum ResourceKind" "\nRenderResult TranslateResult(")
    d3d11_sampler_fragment(D3D11BufferResourceSlot
        "struct ResourceSlot" "\nstruct LegacyTransformConstants")
    d3d11_sampler_fragment(D3D11BufferAdvanceVersion
        "\tvoid advanceBufferContentVersion(" "\n\tstatic bool isElementRangeWithinBuffer(")
    d3d11_sampler_fragment(D3D11BufferUpdateResource
        "\tvirtual RenderResult updateBufferResource(" "\n\tvirtual RenderResult clear(")
    add_executable(core_d3d11_buffer_discard_contract_tests
        D3D11BufferDiscardContractTest.cpp)
    target_include_directories(core_d3d11_buffer_discard_contract_tests PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(core_d3d11_buffer_discard_contract_tests PRIVATE core_renderer)
    set_target_properties(core_d3d11_buffer_discard_contract_tests PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    add_test(NAME core_d3d11_buffer_discard_contract_tests
        COMMAND core_d3d11_buffer_discard_contract_tests)
    set_tests_properties(core_d3d11_buffer_discard_contract_tests PROPERTIES TIMEOUT 30)

    add_executable(core_d3d11_sampler_rollover_tests
        D3D11SamplerRolloverTest.cpp)
    target_include_directories(core_d3d11_sampler_rollover_tests PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(core_d3d11_sampler_rollover_tests PRIVATE core_renderer)
    set_target_properties(core_d3d11_sampler_rollover_tests PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    add_test(NAME core_d3d11_sampler_rollover_tests
        COMMAND core_d3d11_sampler_rollover_tests)
    set_tests_properties(core_d3d11_sampler_rollover_tests PROPERTIES TIMEOUT 30)

    # Exercise the actual backend without pulling the WW3D library into this
    # focused validation/performance fixture.
    add_executable(core_d3d11_index_validation_tests
        D3D11IndexedValidationTest.cpp)
    target_link_libraries(core_d3d11_index_validation_tests PRIVATE core_renderer)
    set_target_properties(core_d3d11_index_validation_tests PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    add_test(NAME core_d3d11_index_validation_tests
        COMMAND core_d3d11_index_validation_tests)
    set_tests_properties(core_d3d11_index_validation_tests PROPERTIES TIMEOUT 30)
endif()

add_test(NAME core_w3d_video_buffer_format_selection_tests
    COMMAND core_renderer_contract_tests --w3d-video-buffer-format-selection
)
set_tests_properties(core_w3d_video_buffer_format_selection_tests PROPERTIES
    TIMEOUT 30
)

# The fixture calls the shared production interval selector and existing
# quaternion math. All three interpolation entrypoints in both titles must
# remain connected to that selector; this supplements, not replaces, execution.
if(WIN32)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/RawAnimationFrameSourceProof.txt" "")
    foreach(raw_title Generals GeneralsMD)
        set(raw_source "${CMAKE_SOURCE_DIR}/${raw_title}/Code/Libraries/Source/WWVegas/WW3D2/hrawanim.cpp")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${raw_source}")
        file(READ "${raw_source}" raw_contents)
        string(REPLACE "\r\n" "\n" raw_contents "${raw_contents}")
        file(SHA256 "${raw_source}" raw_source_hash)
        file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/RawAnimationFrameSourceProof.txt"
            "${raw_title}_source_sha256=${raw_source_hash}\n")
        foreach(raw_method Get_Translation Get_Orientation Get_Transform)
            string(FIND "${raw_contents}" "void HRawAnimClass::${raw_method}(" raw_start)
            if(raw_start LESS 0)
                message(FATAL_ERROR "Raw animation method missing: ${raw_title}/${raw_method}")
            endif()
            string(SUBSTRING "${raw_contents}" ${raw_start} -1 raw_body)
            string(FIND "${raw_body}" "\n}" raw_end)
            if(raw_end LESS 0)
                message(FATAL_ERROR "Raw animation method end missing: ${raw_title}/${raw_method}")
            endif()
            string(SUBSTRING "${raw_body}" 0 ${raw_end} raw_body)
            string(REGEX MATCHALL "rts::animation::SelectRawAnimationFrame0\\(frame\\)" raw_calls "${raw_body}")
            list(LENGTH raw_calls raw_call_count)
            if(NOT raw_call_count EQUAL 1 OR raw_body MATCHES "Float_To_Long")
                message(FATAL_ERROR "Raw interval selector disconnected: ${raw_title}/${raw_method}")
            endif()
            string(SHA256 raw_body_hash "${raw_body}")
            file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/RawAnimationFrameSourceProof.txt"
                "${raw_title}_${raw_method}_sha256=${raw_body_hash}\n")
        endforeach()
    endforeach()
    add_executable(core_raw_animation_frame_tests RawAnimationFrameTest.cpp)
    target_include_directories(core_raw_animation_frame_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas
        ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
        ${CMAKE_SOURCE_DIR}/Dependencies/Utility)
    target_link_libraries(core_raw_animation_frame_tests PRIVATE
        core_wwmath
        core_wwstub
        core_wwdebug
        core_wwlib
        winmm)
    if(IS_VS6_BUILD)
        target_link_libraries(core_raw_animation_frame_tests PRIVATE stlport)
    endif()
    set_target_properties(core_raw_animation_frame_tests PROPERTIES
        CXX_STANDARD 98 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    add_test(NAME core_raw_animation_frame_tests COMMAND core_raw_animation_frame_tests)
    set_tests_properties(core_raw_animation_frame_tests PROPERTIES TIMEOUT 30)
endif()
