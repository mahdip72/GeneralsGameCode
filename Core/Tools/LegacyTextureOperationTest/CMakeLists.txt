add_executable(core_legacy_texture_operation_tests
    LegacyTextureOperationTest.cpp
)

target_include_directories(core_legacy_texture_operation_tests PRIVATE
    ../../Libraries/Include
)

target_compile_definitions(core_legacy_texture_operation_tests PRIVATE
    RTS_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)

if(IS_VS6_BUILD)
    set_target_properties(core_legacy_texture_operation_tests PROPERTIES
        CXX_STANDARD 98
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
else()
    set_target_properties(core_legacy_texture_operation_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
endif()

if(MINGW)
    target_link_options(core_legacy_texture_operation_tests PRIVATE -mconsole)
endif()

add_test(NAME core_legacy_texture_operation_tests
    COMMAND core_legacy_texture_operation_tests
)
set_tests_properties(core_legacy_texture_operation_tests PROPERTIES TIMEOUT 30)
