# The compatibility renderer is an x86/VC6-only build lane.  Keeping this
# topology outside the product source prefixes makes the native x64 graph
# unable to inherit the old device headers, PCH, or import libraries.

function(rts_add_legacy_renderer_targets)
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4 OR
            RTS_VIDEO_BACKEND_GRAPH_AUDIT)
        return()
    endif()

    if(NOT TARGET rts_d3d8_headers OR NOT TARGET rts_d3d8lib)
        message(FATAL_ERROR
            "The x86 legacy renderer requires the D3D8 compatibility targets.")
    endif()

    set(_legacy_root "${CMAKE_SOURCE_DIR}/Core/LegacyRenderer")
    set(_legacy_ww3d2 "${_legacy_root}/WWVegas/WW3D2")
    set(_legacy_gameengine_device "${_legacy_root}/GameEngineDevice")
    set(_product_ww3d2
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WW3D2")

    # These files have no title-local WW3D2 ABI in their translation unit and
    # can therefore be compiled once for both legacy titles.  Keep all
    # wrapper/device implementations out of this target: their historical
    # headers include title-local Camera/Mesh/Shader types and must be built
    # against the exact Generals or Zero Hour include tree below.
    set(_legacy_renderer_sources
        "${_legacy_ww3d2}/dx8fvf.cpp"
        "${_legacy_ww3d2}/formconv_legacy.cpp"
        "${_legacy_ww3d2}/legacytexturecompat.cpp"
        "${_legacy_ww3d2}/surfaceblit_legacy.cpp"
        "${_legacy_ww3d2}/texturemipgenerator_legacy.cpp"
        "${_legacy_gameengine_device}/Source/W3DDevice/Common/System/LegacyPixelShaderBytecode.cpp"
        "${_legacy_gameengine_device}/Source/W3DDevice/GameClient/RenderTextureOperationsLegacy.cpp"
        "${_legacy_gameengine_device}/Source/W3DDevice/GameClient/W3DProfilerFrameCaptureLegacy.cpp"
        "${_legacy_gameengine_device}/Source/W3DDevice/GameClient/W3DSnowLegacy.cpp")

    set(_legacy_title_renderer_sources
        "${_legacy_ww3d2}/d3d11legacybridge.cpp"
        "${_legacy_ww3d2}/dx8caps.cpp"
        "${_legacy_ww3d2}/dx8indexbuffer.cpp"
        "${_legacy_ww3d2}/dx8polygonrenderer.cpp"
        "${_legacy_ww3d2}/dx8renderer.cpp"
        "${_legacy_ww3d2}/dx8rendererdebugger.cpp"
        "${_legacy_ww3d2}/dx8texman.cpp"
        "${_legacy_ww3d2}/dx8vertexbuffer.cpp"
        "${_legacy_ww3d2}/dx8webbrowser.cpp"
        "${_legacy_ww3d2}/dx8wrapper.cpp"
        "${_legacy_ww3d2}/missingtexture_legacy.cpp"
        "${_legacy_ww3d2}/sortingrenderer_legacy.cpp"
        "${_legacy_ww3d2}/surfaceclass_legacy.cpp"
        "${_legacy_ww3d2}/texture_legacy.cpp"
        "${_legacy_ww3d2}/ww3dformat_legacy.cpp")

    add_library(rts_legacy_renderer STATIC ${_legacy_renderer_sources})
    target_include_directories(rts_legacy_renderer BEFORE PUBLIC
        "${_legacy_root}/WWVegas"
        "${_legacy_ww3d2}"
        "${_product_ww3d2}"
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas"
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Source"
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Include"
        "${CMAKE_SOURCE_DIR}/Core/GameEngine/Include"
        "${_legacy_gameengine_device}/Include"
        "${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Include"
        "${CMAKE_SOURCE_DIR}/Dependencies/Utility")
    target_compile_definitions(rts_legacy_renderer PUBLIC
        BUILD_WITH_D3D8=1
        RTS_ENABLE_LEGACY_EMBEDDED_BROWSER=1)
    target_link_libraries(rts_legacy_renderer PUBLIC
        core_config
        core_renderer
        core_task_runtime
        core_browserengine
        core_wwcommon
        core_wwdebug
        core_wwlib
        core_wwmath
        rts_d3d8_headers
        rts_d3d8lib)
    # The x86 compatibility include surface belongs to the external legacy
    # target only; the product WWVegas interface stays backend-neutral on x64.
    target_link_libraries(core_wwcommon INTERFACE rts_d3d8_headers)

    set(_legacy_generals_root "${_legacy_root}/Generals/WW3D2")
    add_library(rts_generals_legacy_renderer STATIC
        "${_legacy_generals_root}/DDSFileLegacy.cpp"
        "${_legacy_generals_root}/RenderGameClientLegacy.cpp"
        "${_legacy_ww3d2}/textureloader_legacy.cpp"
        "${_legacy_ww3d2}/line3d_legacy.cpp"
        ${_legacy_title_renderer_sources})
    target_include_directories(rts_generals_legacy_renderer BEFORE PUBLIC
        "${_legacy_generals_root}"
        "${_legacy_root}/WWVegas"
        "${_legacy_ww3d2}"
        "${CMAKE_SOURCE_DIR}/Generals/Code/Libraries/Source/WWVegas"
        "${CMAKE_SOURCE_DIR}/Generals/Code/Libraries/Source/WWVegas/WW3D2"
        "${CMAKE_SOURCE_DIR}/Generals/Code/GameEngineDevice/Include"
        "${CMAKE_SOURCE_DIR}/Generals/Code/GameEngine/Include"
        "${_product_ww3d2}"
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas"
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Source"
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Include"
        "${CMAKE_SOURCE_DIR}/Core/GameEngine/Include"
        "${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Include"
        "${CMAKE_SOURCE_DIR}/Dependencies/Utility")
    target_compile_definitions(rts_generals_legacy_renderer PUBLIC
        BUILD_WITH_D3D8=1
        RTS_ENABLE_LEGACY_EMBEDDED_BROWSER=1
        RTS_GENERALS=1)
    target_link_libraries(rts_generals_legacy_renderer PUBLIC
        rts_legacy_renderer)

    set(_legacy_zerohour_root "${_legacy_root}/GeneralsMD/WW3D2")
    add_library(rts_zerohour_legacy_renderer STATIC
        "${_legacy_zerohour_root}/DDSFileLegacy.cpp"
        "${_legacy_zerohour_root}/RenderGameClientLegacy.cpp"
        "${_legacy_ww3d2}/textureloader_legacy.cpp"
        "${_legacy_ww3d2}/line3d_legacy.cpp"
        ${_legacy_title_renderer_sources})
    target_include_directories(rts_zerohour_legacy_renderer BEFORE PUBLIC
        "${_legacy_zerohour_root}"
        "${_legacy_root}/WWVegas"
        "${_legacy_ww3d2}"
        "${CMAKE_SOURCE_DIR}/GeneralsMD/Code/Libraries/Source/WWVegas"
        "${CMAKE_SOURCE_DIR}/GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2"
        "${CMAKE_SOURCE_DIR}/GeneralsMD/Code/GameEngineDevice/Include"
        "${CMAKE_SOURCE_DIR}/GeneralsMD/Code/GameEngine/Include"
        "${_product_ww3d2}"
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas"
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Source"
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Include"
        "${CMAKE_SOURCE_DIR}/Core/GameEngine/Include"
        "${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Include"
        "${CMAKE_SOURCE_DIR}/Dependencies/Utility")
    target_compile_definitions(rts_zerohour_legacy_renderer PUBLIC
        BUILD_WITH_D3D8=1
        RTS_ENABLE_LEGACY_EMBEDDED_BROWSER=1
        RTS_ZEROHOUR=1)
    target_link_libraries(rts_zerohour_legacy_renderer PUBLIC
        rts_legacy_renderer)

    # corei_ww3d2 carries the neutral headers and common CPU-side sources.
    # The x86 renderer implementations above provide the moved device-bound
    # definitions without putting them back into that interface source list.
    target_link_libraries(corei_ww3d2 INTERFACE rts_legacy_renderer)
    target_include_directories(corei_ww3d2 BEFORE INTERFACE
        "${_legacy_ww3d2}")
endfunction()

# Attach the title-specific x86 renderer ABI and its historical PCH at the
# target boundary.  The product CMake files call this neutral helper so no
# backend source, include root, or compatibility link can enter the x64 graph.
function(rts_configure_title_ww3d2 target title)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(_legacy_root "${CMAKE_SOURCE_DIR}/Core/LegacyRenderer")
        set(_legacy_ww3d2 "${_legacy_root}/WWVegas/WW3D2")
        set(_legacy_gameengine_device "${_legacy_root}/GameEngineDevice")
        if(title STREQUAL "GENERALS")
            set(_legacy_title "${_legacy_root}/Generals/WW3D2")
            set(_legacy_target rts_generals_legacy_renderer)
        elseif(title STREQUAL "ZEROHOUR")
            set(_legacy_title "${_legacy_root}/GeneralsMD/WW3D2")
            set(_legacy_target rts_zerohour_legacy_renderer)
        else()
            message(FATAL_ERROR "Unknown title renderer ABI: ${title}")
        endif()
        target_include_directories(${target} BEFORE PRIVATE
            "${_legacy_title}"
            "${_legacy_ww3d2}"
            "${_legacy_gameengine_device}/Include")
        target_link_libraries(${target} PRIVATE ${_legacy_target})
        target_precompile_headers(${target} PRIVATE
            [["Utility/CppMacros.h"]]
            [["dx8wrapper.h"]]
            [["WWLib/always.h"]]
            [["WWLib/STLUtils.h"]]
            [["WWLib/win.h"]]
            [["WWLib/WWCommon.h"]]
            [["WWLib/wwstring.h"]]
            <windows.h>)
    else()
        target_precompile_headers(${target} PRIVATE
            [["Utility/CppMacros.h"]]
            [["WWLib/always.h"]]
            [["WWLib/STLUtils.h"]]
            [["WWLib/win.h"]]
            [["WWLib/WWCommon.h"]]
            [["WWLib/wwstring.h"]]
            <windows.h>)
    endif()
endfunction()

# The W3DView authoring tools are Win32-only consumers of the historical
# renderer ABI.  Keep their include precedence and library selection in the
# same architecture-owned helper as the game targets; this prevents a moved
# compatibility header from being found through a product include path.
function(rts_attach_title_legacy_renderer target title)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(_legacy_root "${CMAKE_SOURCE_DIR}/Core/LegacyRenderer")
        set(_legacy_ww3d2 "${_legacy_root}/WWVegas/WW3D2")
        if(title STREQUAL "GENERALS")
            set(_legacy_title "${_legacy_root}/Generals/WW3D2")
            set(_legacy_target rts_generals_legacy_renderer)
        elseif(title STREQUAL "ZEROHOUR")
            set(_legacy_title "${_legacy_root}/GeneralsMD/WW3D2")
            set(_legacy_target rts_zerohour_legacy_renderer)
        else()
            message(FATAL_ERROR "Unknown title legacy renderer ABI: ${title}")
        endif()
        target_include_directories(${target} BEFORE PRIVATE
            "${_legacy_title}"
            "${_legacy_root}/WWVegas"
            "${_legacy_ww3d2}"
            "${_legacy_root}/GameEngineDevice/Include")
        target_link_libraries(${target} PRIVATE ${_legacy_target})
    endif()
endfunction()

function(rts_attach_title_game_engine_device target title)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        if(NOT TARGET ${target})
            return()
        endif()
        set(_legacy_gameengine_device
            "${CMAKE_SOURCE_DIR}/Core/LegacyRenderer/GameEngineDevice")
        target_include_directories(${target} BEFORE PRIVATE
            "${_legacy_gameengine_device}/Include")
        if(title STREQUAL "GENERALS")
            set(_legacy_target rts_generals_legacy_renderer)
        elseif(title STREQUAL "ZEROHOUR")
            set(_legacy_target rts_zerohour_legacy_renderer)
        else()
            message(FATAL_ERROR "Unknown title renderer ABI: ${title}")
        endif()
        target_link_libraries(${target} PUBLIC ${_legacy_target})
    endif()
endfunction()
