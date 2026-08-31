# Socket ownership is native Windows only; never inherit the game's PCH.
add_library(core_network_io STATIC
    NetworkIoOwner.cpp
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include/Lib/NetworkIoOwner.h
)

target_include_directories(core_network_io PUBLIC
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
)

target_link_libraries(core_network_io PRIVATE
    core_task_runtime
    ws2_32
)

set_target_properties(core_network_io PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
