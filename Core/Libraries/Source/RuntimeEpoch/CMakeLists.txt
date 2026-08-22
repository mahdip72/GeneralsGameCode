add_library(core_runtime_epoch_contract STATIC
    RuntimeEpochContract.cpp
)

target_include_directories(core_runtime_epoch_contract PUBLIC
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
)

set_target_properties(core_runtime_epoch_contract PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
