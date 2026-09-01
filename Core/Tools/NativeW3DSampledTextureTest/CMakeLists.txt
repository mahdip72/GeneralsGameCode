add_executable(core_native_w3d_sampled_texture_tests
    NativeW3DSampledTextureTest.cpp
    ../../Libraries/Source/WWVegas/WW3D2/nativew3dsampledtexture.cpp
    ../../Libraries/Source/WWVegas/WW3D2/surfaceblit.cpp
)

target_include_directories(core_native_w3d_sampled_texture_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
    ${CMAKE_SOURCE_DIR}/Dependencies/Utility
)

target_link_libraries(core_native_w3d_sampled_texture_tests PRIVATE
    core_renderer
    core_utility_no_pch
    core_ww3d2_native
)

set_target_properties(core_native_w3d_sampled_texture_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

if(MINGW)
    target_link_options(core_native_w3d_sampled_texture_tests PRIVATE -mconsole)
endif()

add_test(NAME core_native_w3d_sampled_texture_tests
    COMMAND core_native_w3d_sampled_texture_tests
)
set_tests_properties(core_native_w3d_sampled_texture_tests PROPERTIES TIMEOUT 30)
