# Historical Win32 authoring/diagnostic dependency boundary. This target is
# unavailable to every supported product graph.
include_guard(GLOBAL)

if(RTS_BUILD_PRODUCT OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "rts_legacy_tool_runtime is available only to non-product 32-bit tools.")
endif()

add_library(rts_legacy_tool_runtime INTERFACE)
target_link_libraries(rts_legacy_tool_runtime INTERFACE milesstub)

if(NOT RTS_BUILD_OPTION_FFMPEG)
    target_link_libraries(rts_legacy_tool_runtime INTERFACE binkstub)
endif()
