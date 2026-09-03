add_executable(core_crc_snapshot_tests
    DeterministicCrcSnapshotTest.cpp
)
target_link_libraries(core_crc_snapshot_tests PRIVATE
    core_task_runtime
)
if(IS_VS6_BUILD)
    set_target_properties(core_crc_snapshot_tests PROPERTIES
        CXX_STANDARD 98
    )
else()
    set_target_properties(core_crc_snapshot_tests PROPERTIES
        CXX_STANDARD 20
    )
endif()
set_target_properties(core_crc_snapshot_tests PROPERTIES
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
if(MSVC)
    target_compile_options(core_crc_snapshot_tests PRIVATE /W4)
    if(NOT IS_VS6_BUILD)
        target_compile_options(core_crc_snapshot_tests PRIVATE /WX)
    endif()
endif()
if(MINGW)
    target_link_options(core_crc_snapshot_tests PRIVATE -mconsole)
endif()

add_test(NAME core_deterministic_crc_snapshot_tests
    COMMAND core_crc_snapshot_tests
)
set_tests_properties(core_deterministic_crc_snapshot_tests PROPERTIES TIMEOUT 30)
