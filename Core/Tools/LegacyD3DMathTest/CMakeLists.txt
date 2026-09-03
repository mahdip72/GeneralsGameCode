if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    return()
endif()

add_executable(core_legacy_d3d_math_tests
    LegacyD3DMathTest.cpp
)

target_include_directories(core_legacy_d3d_math_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
    ${CMAKE_SOURCE_DIR}/Dependencies/Utility
)

target_link_libraries(core_legacy_d3d_math_tests PRIVATE
    d3d8lib
)

set_target_properties(core_legacy_d3d_math_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

if(MINGW)
    target_link_options(core_legacy_d3d_math_tests PRIVATE -mconsole)
endif()

add_test(NAME core_legacy_d3d_math_tests COMMAND core_legacy_d3d_math_tests)
set_tests_properties(core_legacy_d3d_math_tests PROPERTIES TIMEOUT 60)
