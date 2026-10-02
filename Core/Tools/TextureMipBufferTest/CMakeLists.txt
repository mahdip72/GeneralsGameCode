if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    return()
endif()

add_executable(core_texture_mip_buffer_tests
    TextureMipBufferTest.cpp
    ../../Libraries/Source/WWVegas/WW3D2/bitmaphandler.cpp
    ../../Libraries/Source/WWVegas/WW3D2/texturemipbuffer.cpp
)

target_include_directories(core_texture_mip_buffer_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas
    ${CMAKE_SOURCE_DIR}/Dependencies/Utility
    ${CMAKE_SOURCE_DIR}/GeneralsMD/Code/Libraries/Source/WWVegas
)

target_link_libraries(core_texture_mip_buffer_tests PRIVATE
    core_config
    core_task_runtime
    core_utility
    core_wwstub
    d3d8lib
)

set_target_properties(core_texture_mip_buffer_tests PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

if(MINGW)
    target_link_options(core_texture_mip_buffer_tests PRIVATE -mconsole)
endif()

add_test(NAME core_texture_mip_buffer_tests COMMAND core_texture_mip_buffer_tests)
set_tests_properties(core_texture_mip_buffer_tests PROPERTIES TIMEOUT 30)
