# Minimal package-config fixture for the video-backend CMake graph audit.
# The graph is generated but never linked, so interface-only imported targets
# accurately model the package boundary without requiring a local FFmpeg SDK.
set(FFMPEG_FOUND TRUE)
set(FFMPEG_INCLUDE_DIRS "")
set(FFMPEG_LIBRARY_DIRS "")
set(FFMPEG_LIBRARIES "")

foreach(_ffmpeg_component avcodec avformat avutil swresample swscale)
    if(NOT TARGET FFMPEG::${_ffmpeg_component})
        add_library(FFMPEG::${_ffmpeg_component} INTERFACE IMPORTED)
    endif()
endforeach()

unset(_ffmpeg_component)
