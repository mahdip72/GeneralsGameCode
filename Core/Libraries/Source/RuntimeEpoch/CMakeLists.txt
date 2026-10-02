set(_core_runtime_epoch_contract_sources
    CanonicalSimulationChecksum.cpp
    RuntimeEpochContract.cpp
)

# Lockstep-v2 is part of the installed native x64 proof only.  Keep the
# legacy runtime-epoch library available to every non-VC6 build, but do not
# make the VC6 oracle or a 32-bit compatibility build parse/link this modern
# C++ proof implementation.
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT IS_VS6_BUILD)
    list(APPEND _core_runtime_epoch_contract_sources
        LockstepV2Contract.cpp
    )
endif()

add_library(core_runtime_epoch_contract STATIC
    ${_core_runtime_epoch_contract_sources}
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
