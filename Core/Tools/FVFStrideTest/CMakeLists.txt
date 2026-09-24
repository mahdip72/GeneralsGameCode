add_executable(core_fvf_stride_tests
    FVFStrideTest.cpp
)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_sources(core_fvf_stride_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/Core/LegacyRenderer/WWVegas/WW3D2/dx8fvf.cpp
    )
else()
    target_sources(core_fvf_stride_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WW3D2/nativew3dfvf.cpp
    )
endif()

target_include_directories(core_fvf_stride_tests PRIVATE
    ../../Libraries/Include
    ../../Libraries/Source/WWVegas
)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_include_directories(core_fvf_stride_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/Core/LegacyRenderer/WWVegas
    )
endif()

target_link_libraries(core_fvf_stride_tests PRIVATE
    core_wwlib
    core_wwdebug
    core_wwstub
    corei_always
    winmm
)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_link_libraries(core_fvf_stride_tests PRIVATE d3d8lib)
endif()

if(IS_VS6_BUILD)
    set_target_properties(core_fvf_stride_tests PROPERTIES
        CXX_STANDARD 98
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
else()
    set_target_properties(core_fvf_stride_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
endif()

if(MINGW)
    target_link_options(core_fvf_stride_tests PRIVATE -mconsole)
endif()

add_test(NAME core_fvf_stride_tests COMMAND core_fvf_stride_tests)
set_tests_properties(core_fvf_stride_tests PROPERTIES TIMEOUT 30)
