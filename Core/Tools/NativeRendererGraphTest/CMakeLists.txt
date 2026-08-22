add_executable(core_native_renderer_graph_tests
    NativeRendererGraphTest.cpp
)

target_link_libraries(core_native_renderer_graph_tests PRIVATE
    core_renderer
)

set_target_properties(core_native_renderer_graph_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_native_renderer_graph_tests
    COMMAND core_native_renderer_graph_tests
)
set_tests_properties(core_native_renderer_graph_tests PROPERTIES TIMEOUT 30)
