add_executable(core_test_capacity_lane_tests
    TestCapacityLaneTest.cpp
)
set_target_properties(core_test_capacity_lane_tests PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
if(MSVC)
    target_compile_options(core_test_capacity_lane_tests PRIVATE /W4)
endif()
if(MINGW)
    target_link_options(core_test_capacity_lane_tests PRIVATE -mconsole)
endif()

add_test(NAME core_test_capacity_lane_tests
    COMMAND core_test_capacity_lane_tests
)
set_tests_properties(core_test_capacity_lane_tests PROPERTIES TIMEOUT 30)
