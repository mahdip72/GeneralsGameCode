add_executable(core_task_runtime_tests WIN32)

target_sources(core_task_runtime_tests PRIVATE
    TaskRuntimeTest.cpp
)

target_link_libraries(core_task_runtime_tests PRIVATE
    core_task_runtime
)

set_target_properties(core_task_runtime_tests PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

if(RTS_BUILD_CORE_EXTRAS)
    target_compile_definitions(core_task_runtime_tests PRIVATE
        RTS_BUILD_CORE_EXTRAS
    )
endif()

if(UNIX)
    target_link_libraries(core_task_runtime_tests PRIVATE
        Threads::Threads
    )
endif()

if(WIN32 OR "${CMAKE_SYSTEM}" MATCHES "Windows")
    target_link_options(core_task_runtime_tests PRIVATE /subsystem:console)
endif()

add_test(NAME core_task_runtime_tests COMMAND core_task_runtime_tests)
set_tests_properties(core_task_runtime_tests PROPERTIES TIMEOUT 30)
