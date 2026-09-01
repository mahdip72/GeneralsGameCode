add_executable(core_physics_integration_kernel_tests
    PhysicsIntegrationKernelTest.cpp
)
target_link_libraries(core_physics_integration_kernel_tests PRIVATE core_task_runtime)
if(MSVC)
    target_compile_options(core_physics_integration_kernel_tests PRIVATE /UNDEBUG)
else()
    target_compile_options(core_physics_integration_kernel_tests PRIVATE -UNDEBUG)
endif()
if(RTS_BUILD_CORE_EXTRAS)
    target_compile_definitions(core_physics_integration_kernel_tests PRIVATE
        RTS_BUILD_CORE_EXTRAS
    )
endif()
if(IS_VS6_BUILD)
    set_target_properties(core_physics_integration_kernel_tests PROPERTIES CXX_STANDARD 98)
else()
    set_target_properties(core_physics_integration_kernel_tests PROPERTIES CXX_STANDARD 20)
endif()
set_target_properties(core_physics_integration_kernel_tests PROPERTIES
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
if(MINGW)
    target_link_options(core_physics_integration_kernel_tests PRIVATE -mconsole)
endif()

add_test(NAME core_physics_integration_kernel_tests
    COMMAND core_physics_integration_kernel_tests)
set_tests_properties(core_physics_integration_kernel_tests PROPERTIES TIMEOUT 60)
