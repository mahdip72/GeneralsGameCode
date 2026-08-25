add_library(core_replay_command_contract STATIC
    ReplayCommandContract.cpp
)

target_include_directories(core_replay_command_contract PUBLIC
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
)

set_target_properties(core_replay_command_contract PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
