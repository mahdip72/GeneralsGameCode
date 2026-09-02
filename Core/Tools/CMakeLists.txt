# Build tools

# Build useful tool binaries.
if(RTS_BUILD_CORE_TOOLS)
    add_subdirectory(DebugWindow)
    add_subdirectory(ParticleEditor)
endif()

# Build less useful tool/test binaries.
if(RTS_BUILD_CORE_EXTRAS)
    add_subdirectory(TestSupport)
    if(WIN32 AND NOT IS_VS6_BUILD)
        add_subdirectory(LauncherProcessContractTest)
    endif()
    if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND WIN32 AND NOT IS_VS6_BUILD)
        add_subdirectory(NativeRendererGraphTest)
        add_subdirectory(NativeW3DOwnerQueueTest)
        add_subdirectory(NativeW3DRenderStateTest)
        add_subdirectory(NativeW3DRendererTest)
        add_subdirectory(NativeW3DResourcesTest)
		add_subdirectory(NativeW3DBufferOwnerTest)
		add_subdirectory(NativeW3DSampledTextureTest)
        add_subdirectory(NativeW3DTextureOwnerTest)
        add_subdirectory(NativeW3D2Test)
        add_subdirectory(NativeGameRenderClientLifecycleTest)
        add_subdirectory(NativeSortingRendererTest)
        add_subdirectory(NativeW3DLineTest)
        add_subdirectory(NativeProductDeviceLifecycleTest)
        add_subdirectory(X64PortabilityTest)
        add_subdirectory(FrameTimingDiagnosticsTest)
        add_subdirectory(ReplayFieldReaderTest)
        add_subdirectory(ThreadHandleContractTest)
        add_subdirectory(RegistryHandleContractTest)
        add_subdirectory(RegistryViewContractTest)
        add_subdirectory(VersionHandleContractTest)
        add_subdirectory(XAudio2ContractTest)
        add_subdirectory(XAudio2PcmVoiceTest)
        add_subdirectory(XAudio2AudioServiceTest)
        add_subdirectory(XAudio2AudioOwnerTest)
        add_subdirectory(XAudio2AudioControlsTest)
        add_subdirectory(NativeAudioManagerContractTest)
        add_subdirectory(NativeAudioAssetCatalogTest)
        add_subdirectory(NativeXAudioManagerContractTest)
        if(MSVC AND RTS_BUILD_PRODUCT AND
                (RTS_BUILD_GENERALS OR RTS_BUILD_ZEROHOUR))
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
        set_tests_properties(core_asan_runtime_packaging_tests PROPERTIES TIMEOUT 180)
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
        set(_native_product_runtime_audit_configuration "Release")
        if(CMAKE_CONFIGURATION_TYPES)
            set(_native_product_runtime_audit_configuration "$<CONFIG>")
        elseif(CMAKE_BUILD_TYPE)
            set(_native_product_runtime_audit_configuration "${CMAKE_BUILD_TYPE}")
        endif()
        string(SHA256 _native_product_runtime_audit_source_hash
            "${CMAKE_SOURCE_DIR}|${CMAKE_BINARY_DIR}")
        string(SUBSTRING "${_native_product_runtime_audit_source_hash}" 0 16
            _native_product_runtime_audit_source_hash)
        get_filename_component(_native_product_runtime_audit_fallback_root
            "${CMAKE_SOURCE_DIR}" DIRECTORY)
        set(_native_product_runtime_audit_build_root
            "${_native_product_runtime_audit_fallback_root}/.gga/${_native_product_runtime_audit_source_hash}/${_native_product_runtime_audit_configuration}")
        if(DEFINED ENV{RUNNER_TEMP} AND NOT "$ENV{RUNNER_TEMP}" STREQUAL "")
            set(_native_product_runtime_audit_build_root
                "$ENV{RUNNER_TEMP}/gga/${_native_product_runtime_audit_source_hash}/${_native_product_runtime_audit_configuration}")
        endif()
        set(_native_product_runtime_audit_budget_root
            "${_native_product_runtime_audit_build_root}")
        if(CMAKE_CONFIGURATION_TYPES)
            string(REPLACE "$<CONFIG>" "RelWithDebInfo"
                _native_product_runtime_audit_budget_root
                "${_native_product_runtime_audit_budget_root}")
        endif()
        string(LENGTH "${_native_product_runtime_audit_budget_root}"
            _native_product_runtime_audit_build_root_length)
        if(_native_product_runtime_audit_build_root_length GREATER 80)
            message(FATAL_ERROR
                "Native product audit root exceeds the Windows-safe 80-character budget: ${_native_product_runtime_audit_build_root}")
        endif()
        set(_native_product_runtime_audit_command
            powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/NativeProductRuntimeAudit/NativeProductRuntimeAudit.ps1"
                -SourceRoot "${CMAKE_SOURCE_DIR}"
                -BuildRoot "${_native_product_runtime_audit_build_root}"
                -Generator "${CMAKE_GENERATOR}"
                -CCompiler "${CMAKE_C_COMPILER}"
                -CxxCompiler "${CMAKE_CXX_COMPILER}"
                -RcCompiler "${CMAKE_RC_COMPILER}"
                -ManifestTool "${CMAKE_MT}"
                -FFmpegRoot "${FFMPEG_SDK_ROOT}"
                -Configuration "${_native_product_runtime_audit_configuration}"
        )
        if(CMAKE_GENERATOR_PLATFORM)
            list(APPEND _native_product_runtime_audit_command
                -GeneratorPlatform "${CMAKE_GENERATOR_PLATFORM}")
        endif()
        if(FFMPEG_RUNTIME_DIR)
            list(APPEND _native_product_runtime_audit_command
                -FFmpegRuntimeDir "${FFMPEG_RUNTIME_DIR}")
        endif()
        if(CMAKE_TOOLCHAIN_FILE)
            list(APPEND _native_product_runtime_audit_command
                -ToolchainFile "${CMAKE_TOOLCHAIN_FILE}")
        endif()
        if(VCPKG_INSTALLED_DIR)
            list(APPEND _native_product_runtime_audit_command
                -VcpkgInstalledDir "${VCPKG_INSTALLED_DIR}")
        endif()
        if(VCPKG_TARGET_TRIPLET)
            list(APPEND _native_product_runtime_audit_command
                -VcpkgTargetTriplet "${VCPKG_TARGET_TRIPLET}")
        endif()
        if(VCPKG_OVERLAY_TRIPLETS)
            list(APPEND _native_product_runtime_audit_command
                -VcpkgOverlayTriplets "${VCPKG_OVERLAY_TRIPLETS}")
        endif()
        add_test(NAME core_native_product_runtime_audit
            COMMAND ${_native_product_runtime_audit_command})
        set_tests_properties(core_native_product_runtime_audit PROPERTIES TIMEOUT 3600)
        unset(_native_product_runtime_audit_command)
        unset(_native_product_runtime_audit_budget_root)
        unset(_native_product_runtime_audit_build_root)
        unset(_native_product_runtime_audit_build_root_length)
        unset(_native_product_runtime_audit_configuration)
        unset(_native_product_runtime_audit_fallback_root)
        unset(_native_product_runtime_audit_source_hash)
        add_test(NAME core_native_x64_ci_workflow_audit
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/NativeX64CIWorkflowAudit/NativeX64CIWorkflowAudit.ps1"
                -SourceRoot "${CMAKE_SOURCE_DIR}"
        )
        set_tests_properties(core_native_x64_ci_workflow_audit PROPERTIES TIMEOUT 30)
        add_test(NAME core_native_x64_ci_workflow_audit_self_test
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/NativeX64CIWorkflowAudit/NativeX64CIWorkflowAudit.ps1"
                -SelfTest
        )
        set_tests_properties(core_native_x64_ci_workflow_audit_self_test PROPERTIES TIMEOUT 30)
        if(RTS_BUILD_OPTION_FFMPEG)
            add_test(NAME core_ffmpeg_runtime_cache_tests
                COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/FFmpegRuntimeCacheTest/FFmpegRuntimeCacheTest.ps1"
                    -SourceRoot "${CMAKE_SOURCE_DIR}"
                    -ScratchRoot "${CMAKE_BINARY_DIR}/TestScratch/FFmpegRuntimeCache"
                    -FFmpegRoot "${FFMPEG_SDK_ROOT}"
                    -FFmpegRuntimeDir "${FFMPEG_RUNTIME_DIR}"
            )
            set_tests_properties(core_ffmpeg_runtime_cache_tests PROPERTIES TIMEOUT 120)
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
            add_test(NAME core_ffmpeg_movie_playback_audit_self_test
                COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/FFmpegMoviePlaybackAudit/FFmpegMoviePlaybackAudit.ps1"
                    -SelfTest
            )
            set_tests_properties(core_ffmpeg_movie_playback_audit_self_test PROPERTIES TIMEOUT 30)
        endif()
    endif()
    add_subdirectory(JobSystemTest)
    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT IS_VS6_BUILD)
        add_subdirectory(PerformanceReceiptTest)
    endif()
    add_subdirectory(PipelineExecutionPolicyTest)
    add_subdirectory(DeterministicSimulationTest)
	add_subdirectory(MixedWorkerMultiplayerTest)
	add_subdirectory(ObjectComputationIslandTest)
	add_subdirectory(SimulationPhaseGraphTest)
	add_subdirectory(SimulationPhaseGraphOwnerAdapterTest)
    add_subdirectory(DeterministicCrcStreamTest)
    add_subdirectory(DeterministicCrcRuntimeAdapterTest)
    add_subdirectory(DeterministicCrcSnapshotTest)
    add_subdirectory(DeterministicCrcJobSystemAdapterTest)
    add_subdirectory(ObjectStatusTimerKernelTest)
    add_subdirectory(CollisionCandidateKernelTest)
    add_subdirectory(PhysicsIntegrationKernelTest)
    if(NOT IS_VS6_BUILD)
        add_subdirectory(ImmutableSpatialQueryTest)
    endif()
    add_subdirectory(DeterministicSimulationValidation)
    if(NOT IS_VS6_BUILD)
        add_subdirectory(DeterministicAIPlanningTest)
        add_subdirectory(DeterministicPathSearchTest)
    endif()
    add_subdirectory(ParallelVisibilityTest)
    add_subdirectory(ParallelSkinningTest)
    add_subdirectory(EffectPrepareTest)
    add_subdirectory(ProjectedTerrainGridTest)
    add_subdirectory(WaterPolygonKernelTest)
    add_subdirectory(SortingTriangleKernelTest)
    add_subdirectory(GeometryTriangleDecodeTest)
    if(WIN32 AND NOT IS_VS6_BUILD)
        add_subdirectory(ThreadedRenderDeviceTest)
    endif()
    if(TARGET core_network_io)
        add_subdirectory(NetworkIoOwnerTest)
    endif()
    add_subdirectory(BezierLegacyParityTest)
    if(NOT IS_VS6_BUILD)
        add_subdirectory(ResourceIoPipelineTest)
        add_subdirectory(ModelAssetBytesTest)
        add_subdirectory(RuntimeEpochContractTest)
        if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
            add_subdirectory(NetworkWireContractTest)
        endif()
        add_subdirectory(FileTransferTimeoutTest)
        add_subdirectory(PersistPointerTokenTest)
        add_subdirectory(ChunkIOValueReadTest)
    endif()

    if(WIN32)
        add_test(NAME core_renderer_shutdown_lifecycle_audit
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/RendererShutdownLifecycleAudit/RendererShutdownLifecycleAudit.ps1"
                -SourceRoot "${CMAKE_SOURCE_DIR}"
        )
        set_tests_properties(core_renderer_shutdown_lifecycle_audit PROPERTIES TIMEOUT 30)
        add_test(NAME core_renderer_shutdown_lifecycle_audit_self_test
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/RendererShutdownLifecycleAudit/RendererShutdownLifecycleAudit.ps1"
                -SelfTest
        )
        set_tests_properties(core_renderer_shutdown_lifecycle_audit_self_test PROPERTIES TIMEOUT 30)
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
        add_test(NAME core_legacy_state_invalidation_audit
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/LegacyStateInvalidationAudit/LegacyStateInvalidationAudit.ps1"
                -SourceRoot "${CMAKE_SOURCE_DIR}"
        )
        set_tests_properties(core_legacy_state_invalidation_audit PROPERTIES TIMEOUT 30)
        add_test(NAME core_legacy_state_invalidation_audit_self_test
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/LegacyStateInvalidationAudit/LegacyStateInvalidationAudit.ps1"
                -SelfTest
        )
        set_tests_properties(core_legacy_state_invalidation_audit_self_test PROPERTIES TIMEOUT 30)
        add_test(NAME core_terrain_lod_startup_audit
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/TerrainLodStartupAudit/TerrainLodStartupAudit.ps1"
                -SourceRoot "${CMAKE_SOURCE_DIR}"
        )
        set_tests_properties(core_terrain_lod_startup_audit PROPERTIES TIMEOUT 30)
        add_test(NAME core_terrain_lod_startup_audit_self_test
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/TerrainLodStartupAudit/TerrainLodStartupAudit.ps1"
                -SelfTest
        )
        set_tests_properties(core_terrain_lod_startup_audit_self_test PROPERTIES TIMEOUT 30)
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
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        add_subdirectory(TextureMipBufferTest)
    endif()
    add_subdirectory(TextureMipGeneratorTest)
    add_subdirectory(SurfaceBlitTest)
    add_subdirectory(CameraAudioPolicyTest)
    add_subdirectory(MovieFinalFrameTest)
    add_subdirectory(RendererContractTest)
    add_subdirectory(RenderGameClientCommandSinkTest)
    add_subdirectory(LegacyColorPackingTest)
    add_subdirectory(MouseCursorModeTest)
    add_subdirectory(RenderSubmissionPolicyTest)
    add_subdirectory(ShroudDestinationUpdateTest)
    add_subdirectory(VisualParityAnalyzer)
    add_subdirectory(FVFStrideTest)
    add_subdirectory(LegacyPixelShaderBytecodeTest)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        add_subdirectory(LegacyD3DMathTest)
    endif()
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
    add_test(NAME core_native_game_render_client_lifecycle_audit
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/NativeGameRenderClientLifecycleAudit/NativeGameRenderClientLifecycleAudit.ps1"
            -SourceRoot "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(core_native_game_render_client_lifecycle_audit PROPERTIES TIMEOUT 30)
    add_test(NAME core_native_renderer_graph_audit_self_test
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/NativeRendererGraphAudit/NativeRendererGraphAudit.ps1"
            -SelfTest
    )
    set_tests_properties(core_native_renderer_graph_audit_self_test PROPERTIES TIMEOUT 30)
    if(NOT IS_VS6_BUILD AND RTS_BUILD_PRODUCT AND
            (RTS_BUILD_GENERALS OR RTS_BUILD_ZEROHOUR))
        set(_native_renderer_graph_audit_product_targets)
        set(_native_renderer_graph_audit_migration_targets)
        if(RTS_BUILD_GENERALS)
            list(APPEND _native_renderer_graph_audit_product_targets g_generals)
            list(APPEND _native_renderer_graph_audit_migration_targets
                g_ww3d2 g_gameenginedevice)
        endif()
        if(RTS_BUILD_ZEROHOUR)
            list(APPEND _native_renderer_graph_audit_product_targets z_generals)
            list(APPEND _native_renderer_graph_audit_migration_targets
                z_ww3d2 z_gameenginedevice)
        endif()
        set(_native_renderer_graph_audit_configuration "Release")
        if(CMAKE_CONFIGURATION_TYPES)
            set(_native_renderer_graph_audit_configuration "$<CONFIG>")
        elseif(CMAKE_BUILD_TYPE)
            set(_native_renderer_graph_audit_configuration "${CMAKE_BUILD_TYPE}")
        endif()
        add_test(NAME core_native_renderer_graph_audit
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/NativeRendererGraphAudit/NativeRendererGraphAudit.ps1"
                -SourceRoot "${CMAKE_SOURCE_DIR}"
                -BuildRoot "${CMAKE_BINARY_DIR}"
                -Configuration "${_native_renderer_graph_audit_configuration}"
                -Targets core_renderer,core_ww3d2_native
                -MigrationTargets "${_native_renderer_graph_audit_migration_targets}"
                -ProductTargets "${_native_renderer_graph_audit_product_targets}"
                -AuthorityOnly
        )
        set_tests_properties(core_native_renderer_graph_audit PROPERTIES TIMEOUT 120)
        unset(_native_renderer_graph_audit_configuration)
        unset(_native_renderer_graph_audit_migration_targets)
        unset(_native_renderer_graph_audit_product_targets)
    endif()
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
    add_test(NAME core_network_ownership_lifecycle_audit
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/NetworkOwnershipLifecycleAudit/NetworkOwnershipLifecycleAudit.ps1"
            -SourceRoot "${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME core_network_ownership_lifecycle_audit_self_test
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/NetworkOwnershipLifecycleAudit/NetworkOwnershipLifecycleAudit.ps1"
            -SelfTest
    )
    set_tests_properties(core_network_ownership_lifecycle_audit
        core_network_ownership_lifecycle_audit_self_test PROPERTIES TIMEOUT 30)
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
