add_executable(core_native_w3d_owner_queue_tests
    NativeW3DOwnerQueueTest.cpp
)

target_link_libraries(core_native_w3d_owner_queue_tests PRIVATE
    core_renderer
)

set_target_properties(core_native_w3d_owner_queue_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_native_w3d_owner_queue_tests
    COMMAND core_native_w3d_owner_queue_tests
)
set_tests_properties(core_native_w3d_owner_queue_tests PROPERTIES TIMEOUT 30)
