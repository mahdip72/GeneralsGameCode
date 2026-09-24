add_executable(core_texture_mip_generator_tests
    TextureMipGeneratorTest.cpp
    ../../Libraries/Source/WWVegas/WW3D2/texturemipgenerator.cpp
)

target_include_directories(core_texture_mip_generator_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
    ${CMAKE_SOURCE_DIR}/Dependencies/Utility
)

target_link_libraries(core_texture_mip_generator_tests PRIVATE
    core_utility_no_pch
)

set_target_properties(core_texture_mip_generator_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

if(MINGW)
    target_link_options(core_texture_mip_generator_tests PRIVATE -mconsole)
endif()

add_test(NAME core_texture_mip_generator_tests COMMAND core_texture_mip_generator_tests)
set_tests_properties(core_texture_mip_generator_tests PROPERTIES TIMEOUT 30)
