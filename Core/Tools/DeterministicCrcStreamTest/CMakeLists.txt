add_executable(core_deterministic_crc_stream_tests
    DeterministicCrcStreamTest.cpp
)
target_link_libraries(core_deterministic_crc_stream_tests PRIVATE
    core_task_runtime
)
if(IS_VS6_BUILD)
    set_target_properties(core_deterministic_crc_stream_tests PROPERTIES
        CXX_STANDARD 98
    )
else()
    set_target_properties(core_deterministic_crc_stream_tests PROPERTIES
        CXX_STANDARD 20
    )
endif()
set_target_properties(core_deterministic_crc_stream_tests PROPERTIES
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
if(MSVC)
    target_compile_options(core_deterministic_crc_stream_tests PRIVATE
        /W4
        /WX
    )
endif()
if(MINGW)
    target_link_options(core_deterministic_crc_stream_tests PRIVATE -mconsole)
endif()

add_test(NAME core_deterministic_crc_stream_tests
    COMMAND core_deterministic_crc_stream_tests
)
set_tests_properties(core_deterministic_crc_stream_tests PROPERTIES TIMEOUT 30)
