if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    # The token streams are consumed only by the Win32 compatibility adapter;
    # native x64 must not compile or link this legacy source.
    return()
endif()

add_executable(core_legacy_pixel_shader_bytecode_tests
    LegacyPixelShaderBytecodeTest.cpp
    ${CMAKE_SOURCE_DIR}/Core/LegacyRenderer/GameEngineDevice/Source/W3DDevice/Common/System/LegacyPixelShaderBytecode.cpp
)

target_include_directories(core_legacy_pixel_shader_bytecode_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/LegacyRenderer/GameEngineDevice/Include
)

if(IS_VS6_BUILD)
    set_target_properties(core_legacy_pixel_shader_bytecode_tests PROPERTIES
        CXX_STANDARD 98
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
else()
    set_target_properties(core_legacy_pixel_shader_bytecode_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
endif()

if(MINGW)
    target_link_options(core_legacy_pixel_shader_bytecode_tests PRIVATE -mconsole)
endif()

add_test(NAME core_legacy_pixel_shader_bytecode_tests
    COMMAND core_legacy_pixel_shader_bytecode_tests
)
set_tests_properties(core_legacy_pixel_shader_bytecode_tests PROPERTIES TIMEOUT 30)
