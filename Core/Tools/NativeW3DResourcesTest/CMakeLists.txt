add_executable(core_native_w3d_resources_tests
    NativeW3DResourcesTest.cpp
)

target_link_libraries(core_native_w3d_resources_tests PRIVATE
    core_ww3d2_native
    core_wwstub
    core_wwlib
    winmm
)

set_target_properties(core_native_w3d_resources_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_native_w3d_resources_tests
    COMMAND core_native_w3d_resources_tests
)
set_tests_properties(core_native_w3d_resources_tests PROPERTIES TIMEOUT 30)

add_executable(core_native_d3d11_texture_ownership_tests
    D3D11TextureOwnershipTest.cpp
)

target_link_libraries(core_native_d3d11_texture_ownership_tests PRIVATE
    core_renderer
)

set_target_properties(core_native_d3d11_texture_ownership_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_native_d3d11_texture_ownership_tests
    COMMAND core_native_d3d11_texture_ownership_tests
)
set_tests_properties(core_native_d3d11_texture_ownership_tests PROPERTIES
    TIMEOUT 30)

add_executable(core_native_d3d11_smudge_copy_tests D3D11SmudgeCopyTest.cpp)
target_link_libraries(core_native_d3d11_smudge_copy_tests PRIVATE core_ww3d2_native)
target_include_directories(core_native_d3d11_smudge_copy_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../GameEngineDevice/Include")
set_target_properties(core_native_d3d11_smudge_copy_tests PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
add_test(NAME core_native_d3d11_smudge_copy_tests
    COMMAND core_native_d3d11_smudge_copy_tests)
set_tests_properties(core_native_d3d11_smudge_copy_tests PROPERTIES TIMEOUT 30)

add_executable(core_native_d3d11_copy_recovery_tests D3D11CopyRecoveryTest.cpp)
target_link_libraries(core_native_d3d11_copy_recovery_tests PRIVATE core_renderer)
set_target_properties(core_native_d3d11_copy_recovery_tests PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
add_test(NAME core_native_d3d11_copy_recovery_tests
    COMMAND core_native_d3d11_copy_recovery_tests)
set_tests_properties(core_native_d3d11_copy_recovery_tests PROPERTIES TIMEOUT 60)
