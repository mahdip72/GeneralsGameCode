# Native x64 product dependency boundary. The architecture selector owns the
# consumer-facing rts_product_runtime target; this module contains no Win32
# legacy fallback.
include_guard(GLOBAL)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "rts_native_product_runtime is available only to native x64 Windows builds.")
endif()

if(RTS_BUILD_PRODUCT AND (RTS_BUILD_ZEROHOUR OR RTS_BUILD_GENERALS) AND
        NOT RTS_BUILD_OPTION_FFMPEG)
    message(FATAL_ERROR "Native x64 product targets require the FFmpeg video backend.")
endif()

if(NOT TARGET rts_xaudio2)
    message(FATAL_ERROR
        "rts_native_product_runtime requires the native XAudio2 boundary.")
endif()

add_library(rts_native_product_runtime INTERFACE)
target_link_libraries(rts_native_product_runtime INTERFACE
    core_runtime_epoch_contract
    rts_xaudio2
    bcrypt
    d3d11
    dxgi
    dinput8
    dxguid
)

set(RTS_NATIVE_PRODUCT_REQUIRES_LEGACY_D3D8 OFF CACHE INTERNAL
    "Native product owns its device directly through the D3D11 boundary" FORCE)
set(RTS_NATIVE_PRODUCT_RESOURCE_CLOSURE_COMPLETE ON CACHE INTERNAL
    "Native sampled-texture and surface ownership has crossed the D3D11 boundary" FORCE)

file(GENERATE
    OUTPUT "${CMAKE_BINARY_DIR}/native_product_runtime_link_closure.txt"
    CONTENT "target=rts_native_product_runtime\nlinks=$<JOIN:$<TARGET_PROPERTY:rts_native_product_runtime,INTERFACE_LINK_LIBRARIES>,|>\nrequires_legacy_d3d8=OFF\nresource_closure_complete=ON\n"
)
