add_executable(core_resource_io_pipeline_tests ResourceIoPipelineTest.cpp)
target_link_libraries(core_resource_io_pipeline_tests PRIVATE
    core_config
    core_task_runtime
)
set_target_properties(core_resource_io_pipeline_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
if(MINGW)
    target_link_options(core_resource_io_pipeline_tests PRIVATE -mconsole)
endif()
add_test(NAME core_resource_io_pipeline_tests COMMAND core_resource_io_pipeline_tests)
set_tests_properties(core_resource_io_pipeline_tests PROPERTIES TIMEOUT 30)

if(WIN32)
    # Decode both actual title DDS implementations plus shared Targa/mip code.
    # Their WWLib/D3D8 closure must not enter the device-free Unix pipeline test.
    function(add_resource_texture_decode_test target title product_definition)
        set(_title_wwvegas "${CMAKE_SOURCE_DIR}/${title}/Code/Libraries/Source/WWVegas")
        set(_core_wwvegas "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas")
        add_executable(${target}
            ResourceTextureDecodeTest.cpp
            "${_title_wwvegas}/WW3D2/ddsfile.cpp"
            "${_core_wwvegas}/WW3D2/bitmaphandler.cpp"
            "${_core_wwvegas}/WW3D2/texturemipbuffer.cpp"
            "${_core_wwvegas}/WW3D2/formconv.cpp"
        )
        target_include_directories(${target} PRIVATE
            "${_title_wwvegas}"
            "${_title_wwvegas}/WW3D2"
            "${_core_wwvegas}"
            "${_core_wwvegas}/WW3D2"
            "${CMAKE_SOURCE_DIR}/Core/Libraries/Include"
            "${CMAKE_SOURCE_DIR}/Core/Libraries/Source"
            "${CMAKE_SOURCE_DIR}/Dependencies/Utility"
        )
        target_compile_definitions(${target} PRIVATE ${product_definition}=1)
        target_link_libraries(${target} PRIVATE
            core_config
            core_task_runtime
            core_wwlib
            core_wwmath
            core_wwdebug
            core_wwstub
            core_utility
            d3d8lib
            winmm
        )
        set_target_properties(${target} PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )
        if(MINGW)
            target_link_options(${target} PRIVATE -mconsole)
        endif()
        add_test(NAME ${target} COMMAND ${target})
        set_tests_properties(${target} PROPERTIES TIMEOUT 30)
    endfunction()

    add_resource_texture_decode_test(core_resource_texture_decode_g_tests Generals RTS_GENERALS)
    add_resource_texture_decode_test(core_resource_texture_decode_z_tests GeneralsMD RTS_ZEROHOUR)
endif()
