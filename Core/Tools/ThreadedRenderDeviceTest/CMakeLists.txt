add_executable(core_threaded_render_device_tests ThreadedRenderDeviceTest.cpp)
target_link_libraries(core_threaded_render_device_tests PRIVATE core_renderer core_task_runtime)
target_include_directories(core_threaded_render_device_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Libraries/Source/Renderer")
set_target_properties(core_threaded_render_device_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
add_test(NAME core_threaded_render_device_tests COMMAND core_threaded_render_device_tests)
set_tests_properties(core_threaded_render_device_tests PROPERTIES TIMEOUT 30)
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_test(NAME core_threaded_render_pipeline_stall_trace_tests
        COMMAND core_threaded_render_device_tests --pipeline-stall-trace)
    set_tests_properties(core_threaded_render_pipeline_stall_trace_tests PROPERTIES TIMEOUT 30)
endif()

add_executable(core_threaded_render_device_native_tests ThreadedRenderDeviceNativeTest.cpp)
target_link_libraries(core_threaded_render_device_native_tests PRIVATE core_renderer core_task_runtime user32)
set_target_properties(core_threaded_render_device_native_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
add_test(NAME core_threaded_render_device_native_tests COMMAND core_threaded_render_device_native_tests)
set_tests_properties(core_threaded_render_device_native_tests PROPERTIES TIMEOUT 60)
