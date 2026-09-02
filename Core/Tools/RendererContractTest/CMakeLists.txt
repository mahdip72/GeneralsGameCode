add_executable(core_renderer_contract_tests
    RendererContractTest.cpp
    LegacyResetResourceTest.cpp
    LegacyAsyncFramePolicyTest.cpp
    LegacyAsyncBridgeCompletionTest.cpp
)

# Compile the actual reset/publication methods against device-free doubles so
# failed HRESULTs and retained COM references are exercised, not source-matched.
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
    Core/Libraries/Source/WWVegas/WW3D2/d3d11legacybridge.cpp
    "\tvoid Service_Render_Completions()" "\n\tRenderResult End_Threaded_Frame(")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/LegacyAsyncServiceMethods.inc"
    "${async_service_methods}\n")

renderer_reset_source_fragment(reset_methods
    Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp
    "bool DX8Wrapper::Reset_Device(" "void DX8Wrapper::Release_Device()")
renderer_reset_source_fragment(buffer_methods
    Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp
    "void DX8Wrapper::Track_DX8_Vertex_Buffer(" "HRESULT DX8Wrapper::Draw_DX8_Indexed_Primitive(")
renderer_reset_source_fragment(texture_release
    Core/Libraries/Source/WWVegas/WW3D2/texture.cpp
    "void TextureBaseClass::Release_D3D_Texture()" "void TextureBaseClass::Invalidate_Old_Unused_Textures(")
renderer_reset_source_fragment(texture_set
    Core/Libraries/Source/WWVegas/WW3D2/texture.cpp
    "void TextureBaseClass::Set_D3D_Base_Texture(" "void TextureBaseClass::Load_Locked_Surface()")
renderer_reset_source_fragment(texture_apply
    Core/Libraries/Source/WWVegas/WW3D2/texture.cpp
    "void TextureClass::Apply_New_Surface\n" "void TextureClass::Apply(unsigned int stage)")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/LegacyResetMethods.inc"
    "${reset_methods}\n${buffer_methods}\n${texture_release}\n${texture_set}\n${texture_apply}\n")

# Exercise the real scene acceptance and HRESULT mapping, including queued
# native frames, without starting a game or requiring a graphics device.
renderer_reset_source_fragment(frame_result_mapping
    Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp
    "HRESULT Render_Result_To_HRESULT(" "bool Record_Unavailable_Visible_Submission(")
renderer_reset_source_fragment(end_scene_method
    Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp
    "void DX8Wrapper::End_Scene(bool flip_frames)" "void DX8Wrapper::Flip_To_Primary()")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/LegacyAsyncFrameMethods.inc"
    "${frame_result_mapping}\n${end_scene_method}\n")

# Exercise completion-time target retention and cache rebinding using the
# production bridge methods, independently of a real graphics device.
renderer_reset_source_fragment(async_target_methods
    Core/Libraries/Source/WWVegas/WW3D2/d3d11legacybridge.cpp
    "\tIDirect3DSurface8 *Retain_Target_Surface(" "\n\tvoid Service_Render_Completions()")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/LegacyAsyncTargetMethods.inc"
    "${async_target_methods}\n")

target_link_libraries(core_renderer_contract_tests PRIVATE
    core_renderer
    d3d8lib
)
target_compile_definitions(core_renderer_contract_tests PRIVATE
    RTS_RENDERER_RESET_TEST_LEGACY_FRAGMENT=1
    RTS_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)
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
