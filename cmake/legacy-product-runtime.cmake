# Win32/VC6-only product dependency boundary. Architecture-neutral consumers
# must link rts_product_runtime instead of selecting this target directly.
include_guard(GLOBAL)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "rts_legacy_product_runtime is available only to 32-bit builds.")
endif()

add_library(rts_legacy_product_runtime INTERFACE)
target_link_libraries(rts_legacy_product_runtime INTERFACE
    milesstub
    rts_d3d8lib
)

if(NOT RTS_BUILD_OPTION_FFMPEG)
    target_link_libraries(rts_legacy_product_runtime INTERFACE binkstub)
endif()
