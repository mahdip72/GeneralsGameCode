if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_executable(core_surface_blit_tests
        SurfaceBlitTest.cpp
        ${CMAKE_SOURCE_DIR}/Core/LegacyRenderer/WWVegas/WW3D2/surfaceblit_legacy.cpp
    )

    target_include_directories(core_surface_blit_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/Core/LegacyRenderer/WWVegas
        ${CMAKE_SOURCE_DIR}/Core/Libraries/Source
        ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas
        ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
        ${CMAKE_SOURCE_DIR}/Dependencies/Utility
    )

    target_link_libraries(core_surface_blit_tests PRIVATE
        core_utility_no_pch
        d3d8lib
    )

    target_compile_definitions(core_surface_blit_tests PRIVATE BUILD_WITH_D3D8=1)

    set_target_properties(core_surface_blit_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )

    if(MINGW)
        target_link_options(core_surface_blit_tests PRIVATE -mconsole)
    endif()

    add_test(NAME core_surface_blit_tests COMMAND core_surface_blit_tests)
    set_tests_properties(core_surface_blit_tests PROPERTIES TIMEOUT 30)
endif()

# Compile the same CPU contract without D3D8 headers or libraries.  The legacy
# target above retains adapter coverage; this target proves the native byte
# kernels have no COM dependency.
add_executable(core_surface_blit_cpu_tests
    SurfaceBlitTest.cpp
    ../../Libraries/Source/WWVegas/WW3D2/surfaceblit.cpp
)

target_include_directories(core_surface_blit_cpu_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
    ${CMAKE_SOURCE_DIR}/Dependencies/Utility
)

target_link_libraries(core_surface_blit_cpu_tests PRIVATE
    core_utility_no_pch
)

set_target_properties(core_surface_blit_cpu_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

if(MINGW)
    target_link_options(core_surface_blit_cpu_tests PRIVATE -mconsole)
endif()

add_test(NAME core_surface_blit_cpu_tests COMMAND core_surface_blit_cpu_tests)
set_tests_properties(core_surface_blit_cpu_tests PROPERTIES TIMEOUT 30)
