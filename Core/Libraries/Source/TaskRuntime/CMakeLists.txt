add_library(core_task_runtime STATIC)

target_sources(core_task_runtime PRIVATE
    TaskRuntime.cpp
)

target_include_directories(core_task_runtime PUBLIC
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
)

target_link_libraries(core_task_runtime PRIVATE
    core_config
)

if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(core_task_runtime PRIVATE
        Threads::Threads
    )
endif()
