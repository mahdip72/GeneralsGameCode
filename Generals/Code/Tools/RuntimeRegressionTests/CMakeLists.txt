add_library(g_xcrc OBJECT
    "${CMAKE_SOURCE_DIR}/Core/Tools/XferCrcSnapshotTest/XferCrcSnapshotTest.cpp"
)
target_include_directories(g_xcrc PRIVATE
    "${CMAKE_SOURCE_DIR}/Core/Tools/XferCrcSnapshotTest"
)
target_link_libraries(g_xcrc PRIVATE
    g_gameengine
    gi_always
)
set_target_properties(g_xcrc PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)

add_executable(g_skirmish_ai_runner_contract_tests)

target_sources(g_skirmish_ai_runner_contract_tests PRIVATE
    SkirmishAITestRunnerContractTest.cpp
    $<TARGET_OBJECTS:g_xcrc>
)

target_include_directories(g_skirmish_ai_runner_contract_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/Core/Tools/XferCrcSnapshotTest"
)

target_link_libraries(g_skirmish_ai_runner_contract_tests PRIVATE
    comctl32
    core_debug
    core_profile_legacy
    imm32
    rts_product_runtime
    vfw32
    winmm
    g_gameengine
    g_gameenginedevice
    gi_always
)

add_test(NAME g_skirmish_ai_runner_contract_tests
    COMMAND g_skirmish_ai_runner_contract_tests)
set_tests_properties(g_skirmish_ai_runner_contract_tests PROPERTIES TIMEOUT 30)

add_test(NAME g_xfer_crc_snapshot_tests
    COMMAND g_skirmish_ai_runner_contract_tests --xfer-crc-snapshot)
set_tests_properties(g_xfer_crc_snapshot_tests PROPERTIES TIMEOUT 30)

add_test(NAME g_skirmish_ai_replay_epoch_tests
    COMMAND g_skirmish_ai_runner_contract_tests --skirmish-ai-replay-epoch)
set_tests_properties(g_skirmish_ai_replay_epoch_tests PROPERTIES TIMEOUT 30)

add_test(NAME g_texture_load_queue_contract_tests
    COMMAND g_skirmish_ai_runner_contract_tests --texture-load-queue-contract)
set_tests_properties(g_texture_load_queue_contract_tests PROPERTIES TIMEOUT 30)

if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND WIN32 AND NOT IS_VS6_BUILD)
    include("${CMAKE_SOURCE_DIR}/Core/Tools/Stage5PerformanceFixtureTest/HeadlessMetricStartupTitleTest.cmake")
    rts_add_headless_metric_startup_test(g_skirmish_ai_runner_contract_tests
        Generals g_headless_metric_startup_tests)
    # Exercise the real AudioEventRTS/GameAudio implementations, not the
    # isolated manager fixture's runtime substitutes. The hook only sets
    # deterministic test capacity.
    target_link_libraries(g_skirmish_ai_runner_contract_tests PRIVATE
        rts_native_audio_manager
    )
    target_compile_definitions(g_skirmish_ai_runner_contract_tests PRIVATE
        RTS_NATIVE_AUDIO_TEST_HOOK
    )
    if(RTS_BUILD_OPTION_FFMPEG AND RTS_FFMPEG_RUNTIME_DLLS)
        add_custom_command(TARGET g_skirmish_ai_runner_contract_tests POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                ${RTS_FFMPEG_RUNTIME_DLLS} "$<TARGET_FILE_DIR:g_skirmish_ai_runner_contract_tests>"
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
    endif()
    add_test(NAME g_native_logical_audio_tests
        COMMAND g_skirmish_ai_runner_contract_tests --native-logical-audio)
    set_tests_properties(g_native_logical_audio_tests PROPERTIES TIMEOUT 30)

    add_executable(g_point_group_startup_tests PointGroupStartupTest.cpp)
    target_include_directories(g_point_group_startup_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WW3D2"
    )
    target_link_libraries(g_point_group_startup_tests PRIVATE
        comctl32
        core_debug
        core_profile_legacy
        core_ww3d2_native
        imm32
        rts_product_runtime
        vfw32
        winmm
        g_gameengine
        g_gameenginedevice
        gi_always
    )
    add_test(NAME g_point_group_startup_tests
        COMMAND g_point_group_startup_tests)
    set_tests_properties(g_point_group_startup_tests PROPERTIES TIMEOUT 30)

    add_executable(g_native_render_target_texture_tests
        "${CMAKE_SOURCE_DIR}/Core/Tools/NativeW3DTextureOwnerTest/NativeRenderTargetTextureTest.cpp"
    )
    target_include_directories(g_native_render_target_texture_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WW3D2"
    )
    target_link_libraries(g_native_render_target_texture_tests PRIVATE
        core_wwstub
        core_ww3d2_native
        g_ww3d2
        vfw32
        winmm
    )
    set_target_properties(g_native_render_target_texture_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
    add_test(NAME g_native_render_target_texture_tests
        COMMAND g_native_render_target_texture_tests)
    set_tests_properties(g_native_render_target_texture_tests PROPERTIES TIMEOUT 30)

    add_executable(g_native_line3d_render_tests
        "${CMAKE_SOURCE_DIR}/Core/Tools/NativeW3DLineTest/line3dprod.cpp"
    )
    target_include_directories(g_native_line3d_render_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas"
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WW3D2"
        "${CMAKE_SOURCE_DIR}/Generals/Code/Libraries/Source/WWVegas"
        "${CMAKE_SOURCE_DIR}/Generals/Code/Libraries/Source/WWVegas/WW3D2"
    )
    target_link_libraries(g_native_line3d_render_tests PRIVATE
        core_wwstub
        core_ww3d2_native
        g_ww3d2
        vfw32
        winmm
    )
    set_target_properties(g_native_line3d_render_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
    add_test(NAME g_native_line3d_render_tests
        COMMAND g_native_line3d_render_tests)
    set_tests_properties(g_native_line3d_render_tests PROPERTIES TIMEOUT 60)
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND WIN32 AND NOT IS_VS6_BUILD
        AND RTS_BUILD_OPTION_FFMPEG AND FFMPEG_RUNTIME_DIR)
    # Keep direct build-tree invocations app-local for the production-linked
    # audio path, matching the title's installed runtime closure.
    set_tests_properties(
        g_skirmish_ai_replay_epoch_tests
        g_skirmish_ai_runner_contract_tests
        g_native_logical_audio_tests
        g_texture_load_queue_contract_tests
        PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${FFMPEG_RUNTIME_DIR}"
    )
endif()
