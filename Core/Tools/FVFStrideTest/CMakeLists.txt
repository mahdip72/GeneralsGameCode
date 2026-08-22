add_executable(core_fvf_stride_tests
    FVFStrideTest.cpp
    ../../Libraries/Source/WWVegas/WW3D2/dx8fvf.cpp
)

target_include_directories(core_fvf_stride_tests PRIVATE
    ../../Libraries/Include
    ../../Libraries/Source/WWVegas
)

target_link_libraries(core_fvf_stride_tests PRIVATE
    core_wwlib
    core_wwdebug
    core_wwstub
    corei_always
    d3d8lib
)

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
