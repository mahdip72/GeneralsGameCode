add_executable(core_native_w3d_texture_owner_tests
    NativeW3DTextureOwnerTest.cpp
)

target_link_libraries(core_native_w3d_texture_owner_tests PRIVATE
    core_ww3d2_native
)

set_target_properties(core_native_w3d_texture_owner_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_native_w3d_texture_owner_tests
    COMMAND core_native_w3d_texture_owner_tests
)
set_tests_properties(core_native_w3d_texture_owner_tests PROPERTIES TIMEOUT 30)

add_test(NAME core_native_deferred_texture_retention_cpu
    COMMAND core_native_w3d_texture_owner_tests --deferred-texture-retention-cpu)
set_tests_properties(core_native_deferred_texture_retention_cpu PROPERTIES
    TIMEOUT 30 LABELS "renderer;native;ownership;cpu")
