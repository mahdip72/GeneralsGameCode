add_executable(core_render_game_client_command_sink_tests
    RenderGameClientCommandSinkTest.cpp
)

target_include_directories(core_render_game_client_command_sink_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas
    ${CMAKE_SOURCE_DIR}/Dependencies/Utility
)

if(IS_VS6_BUILD)
    set_target_properties(core_render_game_client_command_sink_tests PROPERTIES
        CXX_STANDARD 98
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
else()
    set_target_properties(core_render_game_client_command_sink_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_link_libraries(core_render_game_client_command_sink_tests PRIVATE
        core_renderer
    )
endif()

add_test(NAME core_render_game_client_command_sink_tests
    COMMAND core_render_game_client_command_sink_tests
)
set_tests_properties(core_render_game_client_command_sink_tests PROPERTIES
    TIMEOUT 30
)
