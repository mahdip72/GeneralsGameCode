add_executable(core_native_w3d2_tests
    NativeW3D2Test.cpp
)

target_link_libraries(core_native_w3d2_tests PRIVATE
    core_ww3d2_native
    core_wwstub
    core_wwdebug
    core_wwlib
    winmm
)

set_target_properties(core_native_w3d2_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_native_w3d2_tests COMMAND core_native_w3d2_tests)
set_tests_properties(core_native_w3d2_tests PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)

add_test(NAME core_native_w3d2_open_recovery_serial
    COMMAND core_native_w3d2_tests --explicit-open-recovery-serial)
add_test(NAME core_native_w3d2_open_recovery_parallel
    COMMAND core_native_w3d2_tests --explicit-open-recovery-parallel)
set_tests_properties(
    core_native_w3d2_open_recovery_serial
    core_native_w3d2_open_recovery_parallel
    PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "renderer;native;recovery"
)

add_test(NAME core_native_w3d2_copy_preflight_serial
    COMMAND core_native_w3d2_tests --copy-active-preflight-serial)
add_test(NAME core_native_w3d2_copy_preflight_parallel
    COMMAND core_native_w3d2_tests --copy-active-preflight-parallel)
set_tests_properties(
    core_native_w3d2_copy_preflight_serial
    core_native_w3d2_copy_preflight_parallel
    PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "renderer;native;copy"
)
