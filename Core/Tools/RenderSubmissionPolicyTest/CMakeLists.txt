add_executable(core_render_submission_policy_tests
    RenderSubmissionPolicyTest.cpp
)

target_include_directories(core_render_submission_policy_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
)

if(IS_VS6_BUILD)
    set_target_properties(core_render_submission_policy_tests PROPERTIES
        CXX_STANDARD 98
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
else()
    set_target_properties(core_render_submission_policy_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
endif()

add_test(NAME core_render_submission_policy_tests
    COMMAND core_render_submission_policy_tests
)
set_tests_properties(core_render_submission_policy_tests PROPERTIES TIMEOUT 30)
