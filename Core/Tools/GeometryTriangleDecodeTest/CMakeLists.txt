add_executable(core_geometry_triangle_decode_tests GeometryTriangleDecodeTest.cpp)
target_link_libraries(core_geometry_triangle_decode_tests PRIVATE core_task_runtime)

if(RTS_BUILD_CORE_EXTRAS)
    target_compile_definitions(core_geometry_triangle_decode_tests PRIVATE
        RTS_BUILD_CORE_EXTRAS)
endif()

if(IS_VS6_BUILD)
    target_link_libraries(core_geometry_triangle_decode_tests PRIVATE stlport)
    set_target_properties(core_geometry_triangle_decode_tests PROPERTIES CXX_STANDARD 98)
else()
    set_target_properties(core_geometry_triangle_decode_tests PROPERTIES CXX_STANDARD 20)
endif()
set_target_properties(core_geometry_triangle_decode_tests PROPERTIES
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
if(MINGW)
    target_link_options(core_geometry_triangle_decode_tests PRIVATE -mconsole)
endif()

add_test(NAME core_geometry_triangle_decode_tests COMMAND core_geometry_triangle_decode_tests)
add_test(NAME core_geometry_triangle_decode_serial_tests
    COMMAND core_geometry_triangle_decode_tests --serial-pipelines)
set_tests_properties(core_geometry_triangle_decode_tests core_geometry_triangle_decode_serial_tests
    PROPERTIES TIMEOUT 60)
