# Historical Win32 authoring/diagnostic dependency boundary. This target is
# unavailable to every supported product graph.
include_guard(GLOBAL)

if(RTS_BUILD_PRODUCT OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "rts_legacy_tool_runtime is available only to non-product 32-bit tools.")
endif()

add_library(rts_legacy_tool_runtime INTERFACE)

# The FFmpeg graph audit intentionally excludes the historical Miles/Bink
# SDK targets.  Preserve the architecture-selected interface target so the
# non-product x86 runtime selector remains valid, but keep the audit graph
# free of dangling plain library names.
if(RTS_VIDEO_BACKEND_GRAPH_AUDIT)
    return()
endif()

target_link_libraries(rts_legacy_tool_runtime INTERFACE milesstub)

if(NOT RTS_BUILD_OPTION_FFMPEG)
    target_link_libraries(rts_legacy_tool_runtime INTERFACE binkstub)
endif()
