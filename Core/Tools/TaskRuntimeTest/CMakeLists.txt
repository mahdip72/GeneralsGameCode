add_executable(core_task_runtime_tests WIN32)

target_sources(core_task_runtime_tests PRIVATE
    TaskRuntimeTest.cpp
)

target_link_libraries(core_task_runtime_tests PRIVATE
    core_task_runtime
)

if(UNIX)
    target_link_libraries(core_task_runtime_tests PRIVATE
        Threads::Threads
    )
endif()

if(WIN32 OR "${CMAKE_SYSTEM}" MATCHES "Windows")
    target_link_options(core_task_runtime_tests PRIVATE /subsystem:console)
endif()

add_test(NAME core_task_runtime_tests COMMAND core_task_runtime_tests)
