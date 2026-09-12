function(add_native_title_camera_adapter_test target_name title_root title_prefix)
    add_executable(${target_name}
        NativeTitleCameraAdapterTest.cpp
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WWStub/wwallocstub.cpp")
    target_precompile_headers(${target_name} PRIVATE [["Utility/CppMacros.h"]])
    target_include_directories(${target_name} PRIVATE
        "${CMAKE_SOURCE_DIR}/${title_root}/Code/Libraries/Source/WWVegas/WW3D2"
        "${CMAKE_SOURCE_DIR}/${title_root}/Code/Libraries/Source/WWVegas"
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WW3D2")
    target_link_libraries(${target_name} PRIVATE
        core_wwstub
        core_ww3d2_native
        ${title_prefix}_ww3d2
        ${title_prefix}_wwcommon
        ${title_prefix}i_always_no_pch
        vfw32
        winmm)
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    add_test(NAME ${target_name} COMMAND ${target_name})
    set_tests_properties(${target_name} PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
endfunction()

if(RTS_BUILD_GENERALS)
    add_native_title_camera_adapter_test(g_native_title_camera_adapter_tests Generals g)
endif()
if(RTS_BUILD_ZEROHOUR)
    add_native_title_camera_adapter_test(z_native_title_camera_adapter_tests GeneralsMD z)
endif()
