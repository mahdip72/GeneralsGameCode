# Compile the production stream and host movie paths against device-free test
# doubles. Extracting their actual definitions keeps the regression test from
# duplicating the completion logic or depending on the proprietary Bink DLL.
function(movie_source_fragment output source start_marker end_marker)
    set(source "${CMAKE_SOURCE_DIR}/${source}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
    file(READ "${source}" contents)
    string(REPLACE "\r\n" "\n" contents "${contents}")
    string(FIND "${contents}" "${start_marker}" start)
    if(start LESS 0)
        message(FATAL_ERROR "Movie regression test start marker missing in ${source}")
    endif()
    string(SUBSTRING "${contents}" ${start} -1 fragment)
    if(NOT "${end_marker}" STREQUAL "")
        string(FIND "${fragment}" "${end_marker}" end)
        if(end LESS 0)
            message(FATAL_ERROR "Movie regression test end marker missing in ${source}")
        endif()
        string(SUBSTRING "${fragment}" 0 ${end} fragment)
    endif()
    set(${output} "${fragment}" PARENT_SCOPE)
endfunction()

movie_source_fragment(video_interface
    Core/GameEngine/Include/GameClient/VideoPlayer.h
    "class VideoBuffer\n" "//===============================\n// VideoStream\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/VideoInterface.inc" "${video_interface}\n")

movie_source_fragment(bink_declaration
    Core/GameEngineDevice/Include/VideoDevice/Bink/BinkVideoPlayer.h
    "class BinkVideoStream : public VideoStream" "//===============================\n// BinkVideoPlayer\n")
movie_source_fragment(bink_methods
    Core/GameEngineDevice/Source/VideoDevice/Bink/BinkVideoPlayer.cpp
    "BinkVideoStream::BinkVideoStream()\n" "")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/BinkStream.inc" "${bink_declaration}\n${bink_methods}\n")

movie_source_fragment(window_update
    Core/GameEngine/Source/GameClient/GUI/WindowVideoManager.cpp
    "void WindowVideoManager::update()\n" "void WindowVideoManager::playMovie(")
movie_source_fragment(window_actions
    Core/GameEngine/Source/GameClient/GUI/WindowVideoManager.cpp
    "void WindowVideoManager::pauseMovie(" "void WindowVideoManager::stopAndRemoveMovie(")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/WindowMovieMethods.inc" "${window_update}\n${window_actions}\n")

movie_source_fragment(display_update
    Core/GameEngine/Source/GameClient/Display.cpp
    "void Display::update()\n" "//============================================================================\n// Display::reset")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/DisplayMovieUpdate.inc" "${display_update}\n")

foreach(title Generals GeneralsMD)
    # Only omit the unrelated UI work after the complete main-movie block.
    movie_source_fragment(ingame_update
        ${title}/Code/GameEngine/Source/GameClient/InGameUI.cpp
        "void InGameUI::update()\n" "\n\tif ( m_cameoVideoStream")
    string(REPLACE "void InGameUI::update()" "void ${title}InGameUI::update()"
        ingame_update "${ingame_update}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${title}MovieUpdate.inc" "${ingame_update}\n}\n")
endforeach()

add_executable(core_movie_final_frame_test MovieFinalFrameTest.cpp)
target_include_directories(core_movie_final_frame_test PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/Dependencies/Utility
)
set_target_properties(core_movie_final_frame_test PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
if(IS_VS6_BUILD)
    target_link_libraries(core_movie_final_frame_test PRIVATE stlport)
endif()
if(MINGW)
    target_link_options(core_movie_final_frame_test PRIVATE -mconsole)
endif()
add_test(NAME core_movie_final_frame_test COMMAND core_movie_final_frame_test)
set_tests_properties(core_movie_final_frame_test PROPERTIES TIMEOUT 30)
