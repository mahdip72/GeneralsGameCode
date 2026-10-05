# Registration snippet for the device-free C++98 shadow upload reuse fixture.
add_executable(core_shadow_second_pass_upload_reuse_tests
    ${CMAKE_CURRENT_LIST_DIR}/ShadowSecondPassUploadReusePolicyTest.cpp)
target_include_directories(core_shadow_second_pass_upload_reuse_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include)
set_target_properties(core_shadow_second_pass_upload_reuse_tests PROPERTIES
    CXX_STANDARD 98 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
add_test(NAME core_shadow_second_pass_upload_reuse_tests
    COMMAND core_shadow_second_pass_upload_reuse_tests)
set_tests_properties(core_shadow_second_pass_upload_reuse_tests PROPERTIES
    TIMEOUT 30)

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(core_shadow_stream_reuse_diagnostics_tests ShadowStreamReuseDiagnosticsTest.cpp)
    target_include_directories(core_shadow_stream_reuse_diagnostics_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/Core/Libraries/Include)
    add_test(NAME core_shadow_stream_reuse_diagnostics_tests
        COMMAND ${CMAKE_COMMAND}
            -DEXECUTABLE=$<TARGET_FILE:core_shadow_stream_reuse_diagnostics_tests>
            -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/shadow-reuse-diagnostics-capture
            -P ${CMAKE_CURRENT_LIST_DIR}/ShadowStreamReuseDiagnosticsVerify.cmake)
    set_tests_properties(core_shadow_stream_reuse_diagnostics_tests PROPERTIES TIMEOUT 30)
endif()
