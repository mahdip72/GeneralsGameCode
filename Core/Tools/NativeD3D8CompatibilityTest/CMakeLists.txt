add_executable(core_native_d3d8_compatibility_test
    NativeD3D8CompatibilityTest.cpp
)

target_link_libraries(core_native_d3d8_compatibility_test PRIVATE
    rts_d3d8_headers
    user32
)
add_dependencies(core_native_d3d8_compatibility_test rts_native_d3dx_stage)

add_test(NAME core_native_d3d8_compatibility_test
    COMMAND core_native_d3d8_compatibility_test
        "$<TARGET_FILE:d3d8to9>"
        "$<TARGET_FILE_DIR:d3d8to9>/D3DCompiler_43.dll"
        "$<TARGET_FILE_DIR:d3d8to9>/D3DX9_43.dll"
)
set_tests_properties(core_native_d3d8_compatibility_test PROPERTIES TIMEOUT 60)

add_test(NAME core_native_d3d8_missing_d3dx_cleanup_test
    COMMAND core_native_d3d8_compatibility_test
        --missing-d3dx
        "$<TARGET_FILE:d3d8to9>"
        "$<TARGET_FILE_DIR:d3d8to9>/D3DCompiler_43.dll"
)
set_tests_properties(core_native_d3d8_missing_d3dx_cleanup_test PROPERTIES TIMEOUT 60)
