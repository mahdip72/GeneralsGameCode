add_library(core_runtime_epoch_contract STATIC
    CanonicalSimulationChecksum.cpp
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

if(RTS_BUILD_CORE_EXTRAS)
    add_executable(core_canonical_simulation_checksum_tests
        CanonicalSimulationChecksumTest.cpp
    )

    target_link_libraries(core_canonical_simulation_checksum_tests PRIVATE
        core_runtime_epoch_contract
    )

    set_target_properties(core_canonical_simulation_checksum_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )

    add_test(NAME core_canonical_simulation_checksum_tests
        COMMAND core_canonical_simulation_checksum_tests
    )
    set_tests_properties(core_canonical_simulation_checksum_tests PROPERTIES TIMEOUT 30)
endif()
