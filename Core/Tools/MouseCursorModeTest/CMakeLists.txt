add_executable(core_mouse_cursor_mode_tests
    MouseCursorModeTest.cpp
)

target_include_directories(core_mouse_cursor_mode_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/GameEngine/Include
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas
    ${CMAKE_SOURCE_DIR}/Dependencies/Utility
)

if(IS_VS6_BUILD)
    set_target_properties(core_mouse_cursor_mode_tests PROPERTIES
        CXX_STANDARD 98
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
else()
    set_target_properties(core_mouse_cursor_mode_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
endif()

if(MINGW)
    target_link_options(core_mouse_cursor_mode_tests PRIVATE -mconsole)
endif()

add_test(NAME core_mouse_cursor_mode_tests COMMAND core_mouse_cursor_mode_tests)
set_tests_properties(core_mouse_cursor_mode_tests PROPERTIES TIMEOUT 30)
