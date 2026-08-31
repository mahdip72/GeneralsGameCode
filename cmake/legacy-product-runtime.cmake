# Temporary compatibility boundary for the Win32 product executables.
#
# Native x64 product work must replace this target as a whole.  Consumers must
# not name individual D3D8, Miles, Bink, or DirectInput dependencies directly,
# otherwise a future native graph can accidentally retain a pointer-sized or
# legacy-SDK dependency through an unrelated subsystem.
add_library(rts_legacy_product_runtime INTERFACE)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_link_libraries(rts_legacy_product_runtime INTERFACE
        milesstub
        rts_d3d8lib
    )

    if(NOT RTS_BUILD_OPTION_FFMPEG)
        target_link_libraries(rts_legacy_product_runtime INTERFACE binkstub)
    endif()
elseif(RTS_BUILD_PRODUCT AND (RTS_BUILD_ZEROHOUR OR RTS_BUILD_GENERALS))
    if(NOT RTS_BUILD_OPTION_FFMPEG)
        message(FATAL_ERROR "Native x64 product targets require the FFmpeg video backend.")
    endif()
    target_link_libraries(rts_legacy_product_runtime INTERFACE
        core_runtime_epoch_contract
        rts_d3d8_headers
        rts_native_d3d8_compat_boundary
        rts_xaudio2
        bcrypt
        d3d11
        dxgi
        dinput8
        dxguid
    )
endif()

file(GENERATE
    OUTPUT "${CMAKE_BINARY_DIR}/native_product_runtime_link_closure.txt"
    CONTENT "target=rts_legacy_product_runtime\nlinks=$<JOIN:$<TARGET_PROPERTY:rts_legacy_product_runtime,INTERFACE_LINK_LIBRARIES>,|>\n"
)
