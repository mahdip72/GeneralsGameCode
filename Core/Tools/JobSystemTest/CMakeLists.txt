add_executable(core_job_system_tests)

target_sources(core_job_system_tests PRIVATE
    JobSystemTest.cpp
)

target_link_libraries(core_job_system_tests PRIVATE
    core_task_runtime
)

if(RTS_BUILD_CORE_EXTRAS)
    target_compile_definitions(core_job_system_tests PRIVATE
        RTS_BUILD_CORE_EXTRAS
    )
endif()

if(IS_VS6_BUILD)
    set_target_properties(core_job_system_tests PROPERTIES
        CXX_STANDARD 98
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
else()
    set_target_properties(core_job_system_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
endif()

if(MINGW)
    target_link_options(core_job_system_tests PRIVATE -mconsole)
endif()

add_test(NAME core_job_system_tests COMMAND core_job_system_tests)
set_tests_properties(core_job_system_tests PROPERTIES TIMEOUT 30)
