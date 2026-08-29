add_executable(core_bezier_legacy_parity_test
    BezierLegacyParityTest.cpp
    "${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/Common/Bezier/BezierSegment.cpp"
    "${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/Common/Bezier/BezFwdIterator.cpp"
)

target_include_directories(core_bezier_legacy_parity_test PRIVATE
    "${CMAKE_SOURCE_DIR}/Core/GameEngine/Include/Precompiled"
    "${CMAKE_SOURCE_DIR}/GeneralsMD/Code/GameEngine/Include"
)

target_link_libraries(core_bezier_legacy_parity_test PRIVATE
    corei_always
    corei_gameengine_include
    corei_libraries_source_wwvegas
)

if(IS_VS6_BUILD)
    # The global /Zm1000 setting over-reserves VC6's limited compiler heap for
    # this small standalone target and can fail before compiling the fixture.
    target_compile_options(core_bezier_legacy_parity_test PRIVATE /Zm200)
    target_link_libraries(core_bezier_legacy_parity_test PRIVATE stlport)
    set_target_properties(core_bezier_legacy_parity_test PROPERTIES
        CXX_STANDARD 98
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
else()
    set_target_properties(core_bezier_legacy_parity_test PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
endif()

add_test(NAME core_bezier_legacy_parity_test
    COMMAND core_bezier_legacy_parity_test
)
set_tests_properties(core_bezier_legacy_parity_test PROPERTIES TIMEOUT 30)

install(TARGETS core_bezier_legacy_parity_test
    RUNTIME DESTINATION tests
    COMPONENT Stage2FocusedTests
)
