# Build tools

# Build useful tool binaries.
if(RTS_BUILD_CORE_TOOLS)
    add_subdirectory(DebugWindow)
    add_subdirectory(ParticleEditor)
endif()

# Build the installed-runtime compatibility probe for native products as well
# as the broader core-extras test lane.
if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND WIN32 AND NOT IS_VS6_BUILD AND
    TARGET d3d8to9 AND (RTS_BUILD_PRODUCT OR RTS_BUILD_CORE_EXTRAS))
    add_subdirectory(NativeD3D8CompatibilityTest)
endif()

# Build less useful tool/test binaries.
if(RTS_BUILD_CORE_EXTRAS)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND WIN32 AND NOT IS_VS6_BUILD)
        add_subdirectory(NativeRendererGraphTest)
        add_subdirectory(NativeW3DOwnerQueueTest)
        add_subdirectory(NativeW3DRenderStateTest)
        add_subdirectory(NativeW3DRendererTest)
        add_subdirectory(NativeW3DResourcesTest)
        add_subdirectory(NativeW3D2Test)
        add_subdirectory(X64PortabilityTest)
        add_subdirectory(ReplayFieldReaderTest)
        add_subdirectory(ThreadHandleContractTest)
        add_subdirectory(RegistryHandleContractTest)
        add_subdirectory(VersionHandleContractTest)
        add_subdirectory(XAudio2ContractTest)
        add_subdirectory(XAudio2PcmVoiceTest)
        add_subdirectory(XAudio2AudioServiceTest)
        add_subdirectory(XAudio2AudioControlsTest)
        add_subdirectory(NativeAudioManagerContractTest)
        add_subdirectory(NativeAudioAssetCatalogTest)
        add_subdirectory(NativeXAudioManagerContractTest)
        add_subdirectory(LauncherProcessContractTest)
        if(MSVC)
            add_test(NAME core_native_product_runtime_packaging_tests
                COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/NativeProductRuntimeAudit/NativeProductRuntimePackaging.Tests.ps1"
                    -ScratchRoot "${CMAKE_BINARY_DIR}/TestScratch/NativeProductRuntimePackaging"
            )
            set_tests_properties(core_native_product_runtime_packaging_tests PROPERTIES TIMEOUT 60)
        endif()
        add_test(NAME core_asan_runtime_packaging_tests
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/AsanRuntimeAudit/AsanRuntimePackaging.Tests.ps1"
                -ScratchRoot "${CMAKE_BINARY_DIR}/TestScratch/AsanRuntimePackaging"
        )
        set_tests_properties(core_asan_runtime_packaging_tests PROPERTIES TIMEOUT 60)
        add_test(NAME core_native_audio_graph_audit
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/NativeAudioGraphAudit/NativeAudioGraphAudit.ps1"
                -SourceRoot "${CMAKE_SOURCE_DIR}"
                -BuildRoot "${CMAKE_BINARY_DIR}"
        )
        set_tests_properties(core_native_audio_graph_audit PROPERTIES TIMEOUT 30)
        add_test(NAME core_replay_recorder_integration_audit
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/ReplayRecorderIntegrationAudit/ReplayRecorderIntegrationAudit.ps1"
                -SourceRoot "${CMAKE_SOURCE_DIR}"
        )
        set_tests_properties(core_replay_recorder_integration_audit PROPERTIES TIMEOUT 30)
        add_test(NAME core_replay_recorder_integration_audit_self_test
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/ReplayRecorderIntegrationAudit/ReplayRecorderIntegrationAudit.ps1"
                -SelfTest
        )
        set_tests_properties(core_replay_recorder_integration_audit_self_test PROPERTIES TIMEOUT 30)
        add_test(NAME core_native_product_runtime_audit
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/NativeProductRuntimeAudit/NativeProductRuntimeAudit.ps1"
                -SourceRoot "${CMAKE_SOURCE_DIR}"
                -BuildRoot "${CMAKE_BINARY_DIR}/Stage3NativeProductGraphAudit"
                -Generator "${CMAKE_GENERATOR}"
                -GeneratorPlatform "${CMAKE_GENERATOR_PLATFORM}"
                -CCompiler "${CMAKE_C_COMPILER}"
                -CxxCompiler "${CMAKE_CXX_COMPILER}"
                -RcCompiler "${CMAKE_RC_COMPILER}"
                -ManifestTool "${CMAKE_MT}"
                -FFmpegRoot "${FFMPEG_ROOT}"
        )
        set_tests_properties(core_native_product_runtime_audit PROPERTIES TIMEOUT 1200)
        if(RTS_BUILD_OPTION_FFMPEG)
            add_subdirectory(FFmpegContractTest)
            add_subdirectory(FFmpegFileBehaviorTest)
            add_subdirectory(FFmpegAudioDecoderTest)
            add_subdirectory(FFmpegMoviePlaybackTest)
            add_test(NAME core_ffmpeg_movie_playback_audit
                COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/FFmpegMoviePlaybackAudit/FFmpegMoviePlaybackAudit.ps1"
                    -SourceRoot "${CMAKE_SOURCE_DIR}"
            )
            set_tests_properties(core_ffmpeg_movie_playback_audit PROPERTIES TIMEOUT 30)
        endif()
    endif()
    add_subdirectory(JobSystemTest)
    if(NOT IS_VS6_BUILD)
        add_subdirectory(RuntimeEpochContractTest)
        add_subdirectory(NetworkWireContractTest)
        add_subdirectory(FileTransferTimeoutTest)
        add_subdirectory(PersistPointerTokenTest)
        add_subdirectory(ChunkIOValueReadTest)
    endif()

    if(WIN32)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT IS_VS6_BUILD)
            add_test(NAME core_native_audio_backend_audit
                COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/NativeAudioBackendAudit/NativeAudioBackendAudit.ps1"
                    -SourceRoot "${CMAKE_SOURCE_DIR}"
            )
            set_tests_properties(core_native_audio_backend_audit PROPERTIES TIMEOUT 30)
            add_test(NAME core_xaudio2_audio_service_audit
                COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/XAudio2AudioServiceAudit/XAudio2AudioServiceAudit.ps1"
                    -SourceRoot "${CMAKE_SOURCE_DIR}"
            )
            set_tests_properties(core_xaudio2_audio_service_audit PROPERTIES TIMEOUT 30)
            add_test(NAME core_native_audio_factory_audit
                COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/NativeAudioFactoryAudit/NativeAudioFactoryAudit.ps1"
                    -SourceRoot "${CMAKE_SOURCE_DIR}"
            )
            set_tests_properties(core_native_audio_factory_audit PROPERTIES TIMEOUT 30)
            add_test(NAME core_native_audio_lifecycle_audit
                COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/NativeAudioLifecycleAudit/NativeAudioLifecycleAudit.ps1"
                    -SourceRoot "${CMAKE_SOURCE_DIR}"
            )
            set_tests_properties(core_native_audio_lifecycle_audit PROPERTIES TIMEOUT 30)
            add_test(NAME core_native_xaudio_manager_audit
                COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/NativeXAudioManagerAudit/NativeXAudioManagerAudit.ps1"
                    -SourceRoot "${CMAKE_SOURCE_DIR}"
            )
            set_tests_properties(core_native_xaudio_manager_audit PROPERTIES TIMEOUT 30)
        endif()
        add_test(NAME core_portability_audit
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/PortabilityAudit/PortabilityAudit.ps1"
                -SourceRoot "${CMAKE_SOURCE_DIR}"
        )
        set_tests_properties(core_portability_audit PROPERTIES TIMEOUT 300)
        add_test(NAME core_portability_audit_self_test
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/PortabilityAudit/PortabilityAudit.Tests.ps1"
                -AuditScript "${CMAKE_CURRENT_SOURCE_DIR}/PortabilityAudit/PortabilityAudit.ps1"
        )
        set_tests_properties(core_portability_audit_self_test PROPERTIES TIMEOUT 120)
        add_test(NAME core_renderer_bypass_audit
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/RendererBypassAudit/RendererBypassAudit.ps1"
                -SourceRoot "${CMAKE_SOURCE_DIR}"
        )
        set_tests_properties(core_renderer_bypass_audit PROPERTIES TIMEOUT 120)
        add_test(NAME core_renderer_bypass_audit_self_test
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/RendererBypassAudit/RendererBypassAudit.ps1"
                -SelfTest
        )
        set_tests_properties(core_renderer_bypass_audit_self_test PROPERTIES TIMEOUT 120)
        add_test(NAME core_native_renderer_boundary_audit
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/NativeRendererBoundaryAudit/NativeRendererBoundaryAudit.ps1"
                -SourceRoot "${CMAKE_SOURCE_DIR}"
        )
        set_tests_properties(core_native_renderer_boundary_audit PROPERTIES TIMEOUT 30)
        add_test(NAME core_native_renderer_boundary_audit_self_test
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/NativeRendererBoundaryAudit/NativeRendererBoundaryAudit.ps1"
                -SelfTest
        )
        set_tests_properties(core_native_renderer_boundary_audit_self_test PROPERTIES TIMEOUT 30)
        add_test(NAME core_legacy_d3dx_service_audit
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/LegacyD3DXServiceAudit/LegacyD3DXServiceAudit.ps1"
                -SourceRoot "${CMAKE_SOURCE_DIR}"
        )
        set_tests_properties(core_legacy_d3dx_service_audit PROPERTIES TIMEOUT 120)
        add_test(NAME core_legacy_product_runtime_audit
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/LegacyProductRuntimeAudit/LegacyProductRuntimeAudit.ps1"
                -SourceRoot "${CMAKE_SOURCE_DIR}"
        )
        set_tests_properties(core_legacy_product_runtime_audit PROPERTIES TIMEOUT 30)
        add_test(NAME core_legacy_product_runtime_audit_self_test
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/LegacyProductRuntimeAudit/LegacyProductRuntimeAudit.ps1"
                -SourceRoot "${CMAKE_SOURCE_DIR}"
                -SelfTest
        )
        set_tests_properties(core_legacy_product_runtime_audit_self_test PROPERTIES TIMEOUT 30)
    endif()
    add_subdirectory(TaskRuntimeTest)
    add_subdirectory(RadarTerrainPrepareTest)
    add_subdirectory(HeightMapTerrainPrepareTest)
    add_subdirectory(HeightMapDynamicLightPrepareTest)
    add_subdirectory(RadarOverlayPrepareTest)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        add_subdirectory(MilesAudioCompletionTest)
    endif()
    add_subdirectory(ScreenshotCodecTest)
    add_subdirectory(TextureMipBufferTest)
    add_subdirectory(TextureMipGeneratorTest)
    add_subdirectory(SurfaceBlitTest)
    add_subdirectory(CameraAudioPolicyTest)
    add_subdirectory(RendererContractTest)
    add_subdirectory(MouseCursorModeTest)
    add_subdirectory(RenderSubmissionPolicyTest)
    add_subdirectory(ShroudDestinationUpdateTest)
    add_subdirectory(VisualParityAnalyzer)
    add_subdirectory(FVFStrideTest)
    add_subdirectory(LegacyPixelShaderBytecodeTest)
    add_subdirectory(LegacyD3DMathTest)
    add_subdirectory(LegacyTextureOperationTest)
    add_subdirectory(LegacyTextureCreationTest)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        # Legacy authoring and diagnostic utilities are outside the native x64
        # product/runtime scope and retain Win32 ABI assumptions.
        add_subdirectory(assetcull)
        add_subdirectory(Babylon)
        add_subdirectory(buildVersionUpdate)
        add_subdirectory(Compress)
        add_subdirectory(CRCDiff)
        add_subdirectory(mangler)
        add_subdirectory(matchbot)
        add_subdirectory(textureCompress)
        add_subdirectory(timingTest)
        add_subdirectory(versionUpdate)
        add_subdirectory(wolSetup)
        add_subdirectory(WW3D)
    endif()
endif()

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_test(NAME core_network_session_token_audit
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/NetworkSessionTokenAudit/NetworkSessionTokenAudit.ps1"
            -SourceRoot "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(core_network_session_token_audit PROPERTIES TIMEOUT 30)
    add_test(NAME core_network_session_token_audit_self_test
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/NetworkSessionTokenAudit/NetworkSessionTokenAudit.ps1"
            -SelfTest
    )
    set_tests_properties(core_network_session_token_audit_self_test PROPERTIES TIMEOUT 30)
endif()

if(WIN32)
    if(NOT IS_VS6_BUILD)
        add_test(NAME core_windows_sdk_tool_discovery_test
            COMMAND "${CMAKE_COMMAND}"
                -DRTS_SOURCE_ROOT=${CMAKE_SOURCE_DIR}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/WindowsSdkToolDiscoveryTest/WindowsSdkToolDiscoveryTest.cmake"
        )
        set_tests_properties(core_windows_sdk_tool_discovery_test PROPERTIES TIMEOUT 30)
    endif()
    add_test(NAME core_video_backend_selection_audit
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/VideoBackendSelectionAudit/VideoBackendSelectionAudit.ps1"
            -SourceRoot "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(core_video_backend_selection_audit PROPERTIES TIMEOUT 30)
    add_test(NAME core_video_backend_selection_audit_self_test
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/VideoBackendSelectionAudit/VideoBackendSelectionAudit.ps1"
            -SourceRoot "${CMAKE_SOURCE_DIR}"
            -SelfTest
    )
    set_tests_properties(core_video_backend_selection_audit_self_test PROPERTIES TIMEOUT 30)
    if(MSVC AND NOT IS_VS6_BUILD)
        add_test(NAME core_video_backend_ffmpeg_graph_audit
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/VideoBackendSelectionAudit/FFmpegGraphAudit.ps1"
                -SourceRoot "${CMAKE_SOURCE_DIR}"
                -BuildRoot "${CMAKE_BINARY_DIR}/Stage3FFmpegGraphAudit"
                -FFmpegConfigDir "${CMAKE_CURRENT_SOURCE_DIR}/VideoBackendSelectionAudit/FFmpegConfig"
                -CMakeCommand "${CMAKE_COMMAND}"
        )
        set_tests_properties(core_video_backend_ffmpeg_graph_audit PROPERTIES TIMEOUT 300)
    endif()
endif()

# Add library interfaces here
if(RTS_BUILD_GENERALS_TOOLS OR RTS_BUILD_ZEROHOUR_TOOLS)
    add_subdirectory(ImagePacker)
    add_subdirectory(MapCacheBuilder)
    add_subdirectory(W3DView)
endif()

# Add library interfaces here
if(RTS_BUILD_GENERALS_EXTRAS OR RTS_BUILD_ZEROHOUR_EXTRAS)
    add_subdirectory(Autorun)
    add_subdirectory(PATCHGET)
endif()

# Native x64 product builds always need the shared launcher implementation,
# even when the optional extras preset is disabled.  Keep Autorun/PATCHGET
# Win32 and extras-only so this does not widen the legacy option behavior.
if(RTS_BUILD_GENERALS_EXTRAS OR RTS_BUILD_ZEROHOUR_EXTRAS OR
        (CMAKE_SIZEOF_VOID_P EQUAL 8 AND RTS_BUILD_PRODUCT))
    add_subdirectory(Launcher)
endif()
