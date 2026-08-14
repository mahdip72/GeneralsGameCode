add_executable(core_texture_mip_buffer_tests
    TextureMipBufferTest.cpp
    ../../Libraries/Source/WWVegas/WW3D2/texturemipbuffer.cpp
)

target_include_directories(core_texture_mip_buffer_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas
    ${CMAKE_SOURCE_DIR}/Dependencies/Utility
)

target_link_libraries(core_texture_mip_buffer_tests PRIVATE
    core_config
    core_task_runtime
    core_utility
    core_wwstub
)

if(MINGW)
    target_link_options(core_texture_mip_buffer_tests PRIVATE -mconsole)
endif()

add_test(NAME core_texture_mip_buffer_tests COMMAND core_texture_mip_buffer_tests)
set_tests_properties(core_texture_mip_buffer_tests PROPERTIES TIMEOUT 30)
