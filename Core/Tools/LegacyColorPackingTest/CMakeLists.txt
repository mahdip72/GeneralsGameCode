add_executable(core_legacy_color_packing_tests
    LegacyColorPackingTest.cpp
)

target_include_directories(core_legacy_color_packing_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
)

set_target_properties(core_legacy_color_packing_tests PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

if(MINGW)
    target_link_options(core_legacy_color_packing_tests PRIVATE -mconsole)
endif()

add_test(NAME core_legacy_color_packing_tests
    COMMAND core_legacy_color_packing_tests
)
set_tests_properties(core_legacy_color_packing_tests PROPERTIES TIMEOUT 30)
