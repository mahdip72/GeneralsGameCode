FetchContent_Declare(
    dx8
    GIT_REPOSITORY https://github.com/TheSuperHackers/min-dx8-sdk.git
    GIT_TAG        7bddff8c01f5fb931c3cb73d4aa8e66d303d97bc
)

FetchContent_MakeAvailable(dx8)

# Historical x86 authoring and diagnostic targets retain the D3D8 SDK surface.
# The root graph never loads this module for a supported product build.
if(NOT TARGET rts_d3d8_headers)
    add_library(rts_d3d8_headers INTERFACE)
    target_compile_definitions(rts_d3d8_headers INTERFACE BUILD_WITH_D3D8)
    target_include_directories(rts_d3d8_headers INTERFACE ${dx8_SOURCE_DIR})
endif()

# Historical tooling does not use the optional D3DX8 helper ABI: its
# texture/mipmap/math paths are implemented in-tree.
# Keep the upstream d3d8lib target intact for authoring tools and differential
# tests that still need D3DX8. Supported product targets cannot see this module.
if(NOT TARGET rts_d3d8lib)
    add_library(rts_d3d8lib INTERFACE)
    target_link_libraries(rts_d3d8lib INTERFACE
        d3d8
        dinput8
        dxguid
    )
    target_compile_definitions(rts_d3d8lib INTERFACE BUILD_WITH_D3D8)
    target_include_directories(rts_d3d8lib INTERFACE
        ${dx8_SOURCE_DIR}
    )
    target_link_directories(rts_d3d8lib BEFORE INTERFACE
        ${dx8_SOURCE_DIR}
    )

    if(MSVC)
        # Visual Studio 2015 and newer need the compatibility library because
        # their CRT no longer exports the legacy stdio symbols used by D3D8.
        # VC6 provides those symbols itself; mixing in the modern shim also
        # introduces unresolved Universal CRT dependencies.
        if(MSVC_VERSION GREATER_EQUAL 1900)
            target_link_libraries(rts_d3d8lib INTERFACE legacy_stdio_definitions)
        endif()
        target_link_options(rts_d3d8lib INTERFACE /NODEFAULTLIB:libci.lib)

        if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "12.0.8804")
            target_include_directories(rts_d3d8lib INTERFACE
                ${dx8_SOURCE_DIR}/extra
            )
        endif()
    endif()
endif()
