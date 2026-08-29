FetchContent_Declare(
    dx8
    GIT_REPOSITORY https://github.com/TheSuperHackers/min-dx8-sdk.git
    GIT_TAG        7bddff8c01f5fb931c3cb73d4aa8e66d303d97bc
)

FetchContent_MakeAvailable(dx8)

# The native D3D11 product still compiles the temporary D3D8 parity backend,
# whose public types are used by legacy render packets.  Keep that source
# compatibility separate from the 32-bit D3D8 import-library dependency.
if(NOT TARGET rts_d3d8_headers)
    add_library(rts_d3d8_headers INTERFACE)
    target_compile_definitions(rts_d3d8_headers INTERFACE BUILD_WITH_D3D8)
    target_include_directories(rts_d3d8_headers INTERFACE ${dx8_SOURCE_DIR})
endif()

# The game runtime uses the D3D8 ABI only as the temporary parity backend.  It
# does not use the optional D3DX8 helper ABI: runtime texture/mipmap/math paths
# are implemented in-tree and the D3D11 bridge owns the visible renderer.
# Keep the upstream d3d8lib target intact for authoring tools and differential
# tests that still need D3DX8, but give product targets a dependency that does
# not pull the obsolete helper library into the game executables.
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
