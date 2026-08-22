# Temporary compatibility boundary for the Win32 product executables.
#
# Native x64 product work must replace this target as a whole.  Consumers must
# not name individual D3D8, Miles, Bink, or DirectInput dependencies directly,
# otherwise a future native graph can accidentally retain a pointer-sized or
# legacy-SDK dependency through an unrelated subsystem.
add_library(rts_legacy_product_runtime INTERFACE)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_link_libraries(rts_legacy_product_runtime INTERFACE
        binkstub
        milesstub
        rts_d3d8lib
    )
elseif(RTS_BUILD_PRODUCT AND (RTS_BUILD_ZEROHOUR OR RTS_BUILD_GENERALS))
    message(FATAL_ERROR
        "Native x64 product targets require the Stage 3 D3D11, XAudio2, and FFmpeg runtime replacements. "
        "Configure the x64 foundation preset until those product dependencies are migrated."
    )
endif()
