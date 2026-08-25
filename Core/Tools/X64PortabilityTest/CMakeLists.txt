add_executable(core_x64_portability_tests
    X64PortabilityTest.cpp
)

target_include_directories(core_x64_portability_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/GameEngine/Include
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas
    ${CMAKE_SOURCE_DIR}/Dependencies/Utility
)

target_link_libraries(core_x64_portability_tests PRIVATE core_wwdebug)

set_target_properties(core_x64_portability_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_x64_portability_tests COMMAND core_x64_portability_tests)
set_tests_properties(core_x64_portability_tests PROPERTIES TIMEOUT 30)
