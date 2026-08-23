find_package(FFMPEG REQUIRED)

function(add_ffmpeg_source_compile_contract target_name title_include_directory)
    # These object libraries intentionally provide compile-only coverage. The
    # runtime contract below links representative SDK entry points without
    # pulling the complete game runtime into this focused test executable.
    add_library(${target_name} OBJECT
        ${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Source/VideoDevice/FFmpeg/FFmpegFile.cpp
        ${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Source/VideoDevice/FFmpeg/FFmpegVideoPlayer.cpp
    )

    target_include_directories(${target_name} PRIVATE
        ${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Include
        ${title_include_directory}
    )

    target_link_libraries(${target_name} PRIVATE
        corei_always
        corei_gameengine_include
        corei_libraries_source_wwvegas
        FFMPEG::avcodec
        FFMPEG::avformat
        FFMPEG::avutil
        FFMPEG::swresample
        FFMPEG::swscale
    )

    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
endfunction()

add_ffmpeg_source_compile_contract(core_g_ffmpeg_source_compile_contract
    ${CMAKE_SOURCE_DIR}/Generals/Code/GameEngine/Include)
add_ffmpeg_source_compile_contract(core_z_ffmpeg_source_compile_contract
    ${CMAKE_SOURCE_DIR}/GeneralsMD/Code/GameEngine/Include)

add_executable(core_ffmpeg_contract_test
    FFmpegContractTest.cpp
)
add_dependencies(core_ffmpeg_contract_test
    core_g_ffmpeg_source_compile_contract
    core_z_ffmpeg_source_compile_contract
)

target_link_libraries(core_ffmpeg_contract_test PRIVATE
    FFMPEG::avcodec
    FFMPEG::avformat
    FFMPEG::avutil
    FFMPEG::swresample
    FFMPEG::swscale
)

set_target_properties(
    core_ffmpeg_contract_test
    PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
)

add_test(NAME core_ffmpeg_contract_test COMMAND core_ffmpeg_contract_test)
set_tests_properties(core_ffmpeg_contract_test PROPERTIES TIMEOUT 30)
if(WIN32 AND FFMPEG_RUNTIME_DIR)
    set_tests_properties(core_ffmpeg_contract_test PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${FFMPEG_RUNTIME_DIR}"
    )
endif()
