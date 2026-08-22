add_executable(core_legacy_texture_creation_tests
    LegacyTextureCreationTest.cpp
    LegacyTextureCreationTestDecoderStub.cpp
    ../../Libraries/Source/WWVegas/WW3D2/legacytexturecompat.cpp
    ../../Libraries/Source/WWVegas/WW3D2/surfaceblit.cpp
)

target_include_directories(core_legacy_texture_creation_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
    ${CMAKE_SOURCE_DIR}/Dependencies/Utility
)

target_link_libraries(core_legacy_texture_creation_tests PRIVATE
    core_utility_no_pch
    d3d8lib
)

if (MSVC AND NOT IS_VS6_BUILD)
    target_link_libraries(core_legacy_texture_creation_tests PRIVATE
        ole32
        windowscodecs
    )
endif()

set_target_properties(core_legacy_texture_creation_tests PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

if(MINGW)
    target_link_options(core_legacy_texture_creation_tests PRIVATE -mconsole)
endif()

add_test(NAME core_legacy_texture_creation_tests
    COMMAND core_legacy_texture_creation_tests)
set_tests_properties(core_legacy_texture_creation_tests PROPERTIES TIMEOUT 30)
