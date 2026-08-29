add_executable(core_native_w3d2_tests
    NativeW3D2Test.cpp
)

target_link_libraries(core_native_w3d2_tests PRIVATE
    core_ww3d2_native
)

set_target_properties(core_native_w3d2_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_native_w3d2_tests COMMAND core_native_w3d2_tests)
set_tests_properties(core_native_w3d2_tests PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
