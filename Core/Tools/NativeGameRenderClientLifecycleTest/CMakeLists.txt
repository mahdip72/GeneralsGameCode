function(add_native_game_render_client_lifecycle_test target_name title_ww3d2)
    add_executable(${target_name}
        NativeGameRenderClientLifecycleTest.cpp
    )

    target_include_directories(${target_name} PRIVATE
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WW3D2"
    )

    # The facade translation unit also contains the legacy TextureClass and
    # SurfaceClass forwarding entry points.  Link the matching title WW3D2
    # library so those real title-backed definitions are present; the stub
    # core-only target cannot satisfy that ABI.  This subdirectory is parsed
    # before the product targets, so select them from the configured product
    # options rather than probing TARGET existence here.
    target_link_libraries(${target_name} PRIVATE
        core_wwstub
        core_ww3d2_native
        ${title_ww3d2}
        vfw32
        winmm
    )

    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )

    add_test(NAME ${target_name}
        COMMAND ${target_name})
    set_tests_properties(${target_name} PROPERTIES
        TIMEOUT 30
        SKIP_RETURN_CODE 77)
endfunction()

if(RTS_BUILD_GENERALS)
    add_native_game_render_client_lifecycle_test(
        g_native_game_render_client_lifecycle_tests g_ww3d2)
endif()

if(RTS_BUILD_ZEROHOUR)
    add_native_game_render_client_lifecycle_test(
        z_native_game_render_client_lifecycle_tests z_ww3d2)
endif()
