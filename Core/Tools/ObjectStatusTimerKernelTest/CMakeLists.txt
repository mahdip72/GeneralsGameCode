add_executable(core_object_status_timer_kernel_tests
    ObjectStatusTimerKernelTest.cpp
)
target_link_libraries(core_object_status_timer_kernel_tests PRIVATE core_task_runtime)
if(RTS_BUILD_CORE_EXTRAS)
    target_compile_definitions(core_object_status_timer_kernel_tests PRIVATE
        RTS_BUILD_CORE_EXTRAS
    )
endif()
if(IS_VS6_BUILD)
    set_target_properties(core_object_status_timer_kernel_tests PROPERTIES CXX_STANDARD 98)
else()
    set_target_properties(core_object_status_timer_kernel_tests PROPERTIES CXX_STANDARD 20)
endif()
set_target_properties(core_object_status_timer_kernel_tests PROPERTIES
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
if(MINGW)
    target_link_options(core_object_status_timer_kernel_tests PRIVATE -mconsole)
endif()

add_test(NAME core_object_status_timer_kernel_tests
    COMMAND core_object_status_timer_kernel_tests)
set_tests_properties(core_object_status_timer_kernel_tests PROPERTIES TIMEOUT 30)
