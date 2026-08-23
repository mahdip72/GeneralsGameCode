# Build tools

# Build useful tool binaries.
if(RTS_BUILD_CORE_TOOLS)
    add_subdirectory(DebugWindow)
    add_subdirectory(ParticleEditor)
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
        add_subdirectory(ThreadHandleContractTest)
        add_subdirectory(RegistryHandleContractTest)
        add_subdirectory(VersionHandleContractTest)
        add_subdirectory(XAudio2ContractTest)
        add_subdirectory(XAudio2PcmVoiceTest)
        add_subdirectory(XAudio2AudioServiceTest)
        if(RTS_BUILD_OPTION_FFMPEG)
            add_subdirectory(FFmpegContractTest)
            add_subdirectory(FFmpegFileBehaviorTest)
            add_subdirectory(FFmpegAudioDecoderTest)
        endif()
    endif()
    add_subdirectory(JobSystemTest)
    if(NOT IS_VS6_BUILD)
        add_subdirectory(RuntimeEpochContractTest)
        add_subdirectory(NetworkWireContractTest)
        add_subdirectory(PersistPointerTokenTest)
        add_subdirectory(ChunkIOValueReadTest)
    endif()

    if(WIN32)
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
        add_test(NAME core_portability_audit
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/PortabilityAudit/PortabilityAudit.ps1"
                -SourceRoot "${CMAKE_SOURCE_DIR}"
        )
        set_tests_properties(core_portability_audit PROPERTIES TIMEOUT 120)
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
    add_subdirectory(MilesAudioCompletionTest)
    add_subdirectory(ScreenshotCodecTest)
    add_subdirectory(TextureMipBufferTest)
    add_subdirectory(TextureMipGeneratorTest)
    add_subdirectory(SurfaceBlitTest)
    add_subdirectory(assetcull)
    add_subdirectory(Babylon)
    add_subdirectory(buildVersionUpdate)
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

if(WIN32)
    add_test(NAME core_video_backend_selection_audit
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/VideoBackendSelectionAudit/VideoBackendSelectionAudit.ps1"
            -SourceRoot "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(core_video_backend_selection_audit PROPERTIES TIMEOUT 30)
    if(MSVC AND NOT IS_VS6_BUILD)
        add_test(NAME core_video_backend_ffmpeg_graph_audit
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/VideoBackendSelectionAudit/FFmpegGraphAudit.ps1"
                -SourceRoot "${CMAKE_SOURCE_DIR}"
                -BuildRoot "${CMAKE_BINARY_DIR}/Stage3FFmpegGraphAudit"
                -FFmpegConfigDir "${CMAKE_CURRENT_SOURCE_DIR}/VideoBackendSelectionAudit/FFmpegConfig"
                -CMakeCommand "${CMAKE_COMMAND}"
        )
        set_tests_properties(core_video_backend_ffmpeg_graph_audit PROPERTIES TIMEOUT 120)
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
    add_subdirectory(Launcher)
    add_subdirectory(PATCHGET)
endif()
