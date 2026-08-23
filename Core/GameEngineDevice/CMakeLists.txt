set(GAMEENGINEDEVICE_SRC
    Include/W3DDevice/Common/W3DConvert.h
    Include/W3DDevice/Common/W3DFunctionLexicon.h
    Include/W3DDevice/Common/W3DModuleFactory.h
    Include/W3DDevice/Common/RadarOverlayPrepare.h
    Include/W3DDevice/Common/RadarTerrainPrepare.h
    Include/W3DDevice/Common/HeightMapTerrainPrepare.h
    Include/W3DDevice/Common/HeightMapDynamicLightPrepare.h
    Include/W3DDevice/Common/LegacyPixelShaderBytecode.h
    Include/W3DDevice/Common/W3DRadar.h
    Include/W3DDevice/Common/W3DShroudRenderPolicy.h
    Include/W3DDevice/Common/W3DThingFactory.h
    Include/W3DDevice/GameClient/BaseHeightMap.h
#    Include/W3DDevice/GameClient/camerashakesystem.h
    Include/W3DDevice/GameClient/FlatHeightMap.h
    Include/W3DDevice/GameClient/HeightMap.h
    Include/W3DDevice/GameClient/Module/W3DDebrisDraw.h
    Include/W3DDevice/GameClient/Module/W3DDefaultDraw.h
    Include/W3DDevice/GameClient/Module/W3DDependencyModelDraw.h
    Include/W3DDevice/GameClient/Module/W3DLaserDraw.h
    Include/W3DDevice/GameClient/Module/W3DModelDraw.h
    Include/W3DDevice/GameClient/Module/W3DOverlordAircraftDraw.h
    Include/W3DDevice/GameClient/Module/W3DOverlordTankDraw.h
    Include/W3DDevice/GameClient/Module/W3DOverlordTruckDraw.h
    Include/W3DDevice/GameClient/Module/W3DPoliceCarDraw.h
    Include/W3DDevice/GameClient/Module/W3DProjectileStreamDraw.h
    Include/W3DDevice/GameClient/Module/W3DPropDraw.h
    Include/W3DDevice/GameClient/Module/W3DRopeDraw.h
    Include/W3DDevice/GameClient/Module/W3DScienceModelDraw.h
    Include/W3DDevice/GameClient/Module/W3DSupplyDraw.h
    Include/W3DDevice/GameClient/Module/W3DTankDraw.h
    Include/W3DDevice/GameClient/Module/W3DTankTruckDraw.h
    Include/W3DDevice/GameClient/Module/W3DTracerDraw.h
    Include/W3DDevice/GameClient/Module/W3DTreeDraw.h
    Include/W3DDevice/GameClient/Module/W3DTruckDraw.h
    Include/W3DDevice/GameClient/TerrainTex.h
    Include/W3DDevice/GameClient/TileData.h
#    Include/W3DDevice/GameClient/W3DAssetManager.h
#    Include/W3DDevice/GameClient/W3DAssetManagerExposed.h
#    Include/W3DDevice/GameClient/W3DBibBuffer.h
#    Include/W3DDevice/GameClient/W3DBridgeBuffer.h
#    Include/W3DDevice/GameClient/W3DBufferManager.h
#    Include/W3DDevice/GameClient/W3DCustomScene.h
#    Include/W3DDevice/GameClient/W3DDebugDisplay.h
#    Include/W3DDevice/GameClient/W3DDebugIcons.h
#    Include/W3DDevice/GameClient/W3DDisplay.h
#    Include/W3DDevice/GameClient/W3DDisplayString.h
#    Include/W3DDevice/GameClient/W3DDisplayStringManager.h
#    Include/W3DDevice/GameClient/W3DDynamicLight.h
#    Include/W3DDevice/GameClient/W3DFileSystem.h
    Include/W3DDevice/GameClient/W3DGadget.h
#    Include/W3DDevice/GameClient/W3DGameClient.h
    Include/W3DDevice/GameClient/W3DGameFont.h
    Include/W3DDevice/GameClient/W3DGameWindow.h
    Include/W3DDevice/GameClient/W3DGameWindowManager.h
    Include/W3DDevice/GameClient/W3DGUICallbacks.h
#    Include/W3DDevice/GameClient/W3DInGameUI.h
#    Include/W3DDevice/GameClient/W3DMirror.h
    Include/W3DDevice/GameClient/W3DMouse.h
#    Include/W3DDevice/GameClient/W3DParticleSys.h
#    Include/W3DDevice/GameClient/W3DPoly.h
    Include/W3DDevice/GameClient/W3DProfilerFrameCapture.h
#    Include/W3DDevice/GameClient/W3DProjectedShadow.h
    Include/W3DDevice/GameClient/W3DPropBuffer.h
    Include/W3DDevice/GameClient/W3DScorch.h
#    Include/W3DDevice/GameClient/W3DRoadBuffer.h
#    Include/W3DDevice/GameClient/W3DScene.h
    Include/W3DDevice/GameClient/W3DShaderManager.h
#    Include/W3DDevice/GameClient/W3DShadow.h
#    Include/W3DDevice/GameClient/W3DShroud.h
    Include/W3DDevice/GameClient/W3DSmudge.h
    Include/W3DDevice/GameClient/W3DSnow.h
#    Include/W3DDevice/GameClient/W3DStatusCircle.h
    Include/W3DDevice/GameClient/W3DTerrainBackground.h
    Include/W3DDevice/GameClient/W3DTerrainTracks.h
    Include/W3DDevice/GameClient/W3DTerrainVisual.h
    Include/W3DDevice/GameClient/W3DTreeBuffer.h
    Include/W3DDevice/GameClient/W3DVideoBuffer.h
    Include/W3DDevice/GameClient/W3DView.h
    Include/W3DDevice/GameClient/TerrainDrawSizing.h
    Include/W3DDevice/GameClient/W3DScreenshot.h
    Include/W3DDevice/GameClient/W3DScreenshotCodec.h
#    Include/W3DDevice/GameClient/W3DVolumetricShadow.h
    Include/W3DDevice/GameClient/W3DWater.h
    Include/W3DDevice/GameClient/W3DWaterTracks.h
#    Include/W3DDevice/GameClient/W3DWaypointBuffer.h
#    Include/W3DDevice/GameClient/W3DWebBrowser.h
    Include/W3DDevice/GameClient/WorldHeightMap.h
#    Include/W3DDevice/GameLogic/W3DGameLogic.h
#    Include/W3DDevice/GameLogic/W3DGhostObject.h
#    Include/W3DDevice/GameLogic/W3DTerrainLogic.h
    Include/Win32Device/Common/Win32BIGFile.h
    Include/Win32Device/Common/Win32BIGFileSystem.h
    Include/Win32Device/Common/Win32GameEngine.h
    Include/Win32Device/Common/Win32LocalFile.h
    Include/Win32Device/Common/Win32LocalFileSystem.h
    Include/Win32Device/GameClient/Win32DIKeyboard.h
    #Include/Win32Device/GameClient/Win32DIMouse.h
    Include/Win32Device/GameClient/Win32Mouse.h
    Source/W3DDevice/Common/System/W3DFunctionLexicon.cpp
    Source/W3DDevice/Common/System/RadarOverlayPrepare.cpp
    Source/W3DDevice/Common/System/RadarTerrainPrepare.cpp
    Source/W3DDevice/Common/System/HeightMapTerrainPrepare.cpp
    Source/W3DDevice/Common/System/HeightMapDynamicLightPrepare.cpp
    Source/W3DDevice/Common/System/LegacyPixelShaderBytecode.cpp
    Source/W3DDevice/Common/System/W3DRadar.cpp
    Source/W3DDevice/Common/Thing/W3DModuleFactory.cpp
    Source/W3DDevice/Common/Thing/W3DThingFactory.cpp
    Source/W3DDevice/Common/W3DConvert.cpp
    Source/W3DDevice/Common/System/W3DShroudRenderPolicy.cpp
    Source/W3DDevice/GameClient/BaseHeightMap.cpp
    Source/W3DDevice/GameClient/CameraShakeSystem.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DDebrisDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DDefaultDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DDependencyModelDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DLaserDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DModelDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DOverlordAircraftDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DOverlordTankDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DOverlordTruckDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DPoliceCarDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DProjectileStreamDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DPropDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DRopeDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DScienceModelDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DSupplyDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DTankDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DTankTruckDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DTracerDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DTreeDraw.cpp
    Source/W3DDevice/GameClient/Drawable/Draw/W3DTruckDraw.cpp
    Source/W3DDevice/GameClient/FlatHeightMap.cpp
    Source/W3DDevice/GameClient/GUI/Gadget/W3DCheckBox.cpp
    Source/W3DDevice/GameClient/GUI/Gadget/W3DComboBox.cpp
    Source/W3DDevice/GameClient/GUI/Gadget/W3DHorizontalSlider.cpp
    Source/W3DDevice/GameClient/GUI/Gadget/W3DListBox.cpp
    Source/W3DDevice/GameClient/GUI/Gadget/W3DProgressBar.cpp
    Source/W3DDevice/GameClient/GUI/Gadget/W3DPushButton.cpp
    Source/W3DDevice/GameClient/GUI/Gadget/W3DRadioButton.cpp
    Source/W3DDevice/GameClient/GUI/Gadget/W3DStaticText.cpp
    Source/W3DDevice/GameClient/GUI/Gadget/W3DTabControl.cpp
    Source/W3DDevice/GameClient/GUI/Gadget/W3DTextEntry.cpp
    Source/W3DDevice/GameClient/GUI/Gadget/W3DVerticalSlider.cpp
    Source/W3DDevice/GameClient/GUI/GUICallbacks/W3DControlBar.cpp
    Source/W3DDevice/GameClient/GUI/GUICallbacks/W3DMainMenu.cpp
    Source/W3DDevice/GameClient/GUI/GUICallbacks/W3DMOTD.cpp
    Source/W3DDevice/GameClient/GUI/W3DGameFont.cpp
    Source/W3DDevice/GameClient/GUI/W3DGameWindow.cpp
    Source/W3DDevice/GameClient/GUI/W3DGameWindowManager.cpp
    Source/W3DDevice/GameClient/HeightMap.cpp
#    Source/W3DDevice/GameClient/Shadow/W3DBufferManager.cpp
#    Source/W3DDevice/GameClient/Shadow/W3DProjectedShadow.cpp
#    Source/W3DDevice/GameClient/Shadow/W3DShadow.cpp
#    Source/W3DDevice/GameClient/Shadow/W3DVolumetricShadow.cpp
    Source/W3DDevice/GameClient/TerrainTex.cpp
    Source/W3DDevice/GameClient/TileData.cpp
#    Source/W3DDevice/GameClient/W3DAssetManager.cpp
#    Source/W3DDevice/GameClient/W3DAssetManagerExposed.cpp
#    Source/W3DDevice/GameClient/W3DBibBuffer.cpp
#    Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp
#    Source/W3DDevice/GameClient/W3DDebugDisplay.cpp
#    Source/W3DDevice/GameClient/W3DDebugIcons.cpp
#    Source/W3DDevice/GameClient/W3DDisplay.cpp
#    Source/W3DDevice/GameClient/W3DDisplayString.cpp
#    Source/W3DDevice/GameClient/W3DDisplayStringManager.cpp
#    Source/W3DDevice/GameClient/W3DDynamicLight.cpp
#    Source/W3DDevice/GameClient/W3DFileSystem.cpp
#    Source/W3DDevice/GameClient/W3DGameClient.cpp
#    Source/W3DDevice/GameClient/W3DInGameUI.cpp
    Source/W3DDevice/GameClient/W3DMouse.cpp
    Source/W3DDevice/GameClient/W3DParticleSys.cpp
#    Source/W3DDevice/GameClient/W3DPoly.cpp
    Source/W3DDevice/GameClient/W3DProfilerFrameCapture.cpp
    Source/W3DDevice/GameClient/W3DPropBuffer.cpp
    Source/W3DDevice/GameClient/W3DScorch.cpp
#    Source/W3DDevice/GameClient/W3DRoadBuffer.cpp
#    Source/W3DDevice/GameClient/W3DScene.cpp
    Source/W3DDevice/GameClient/W3DShaderManager.cpp
#    Source/W3DDevice/GameClient/W3DShroud.cpp
    Source/W3DDevice/GameClient/W3DSmudge.cpp
    Source/W3DDevice/GameClient/W3DSnow.cpp
#    Source/W3DDevice/GameClient/W3DStatusCircle.cpp
    Source/W3DDevice/GameClient/W3DTerrainBackground.cpp
    Source/W3DDevice/GameClient/W3DTerrainTracks.cpp
    Source/W3DDevice/GameClient/W3DTerrainVisual.cpp
    Source/W3DDevice/GameClient/W3DTreeBuffer.cpp
    Source/W3DDevice/GameClient/W3DVideoBuffer.cpp
    Source/W3DDevice/GameClient/W3DView.cpp
    Source/W3DDevice/GameClient/TerrainDrawSizing.cpp
    Source/W3DDevice/GameClient/W3DScreenshot.cpp
    Source/W3DDevice/GameClient/W3DScreenshotCodec.cpp
    Source/W3DDevice/GameClient/stb_image_write_impl.cpp
#    Source/W3DDevice/GameClient/W3dWaypointBuffer.cpp
#    Source/W3DDevice/GameClient/W3DWebBrowser.cpp
    Source/W3DDevice/GameClient/Water/W3DWater.cpp
    Source/W3DDevice/GameClient/Water/W3DWaterTracks.cpp
    Source/W3DDevice/GameClient/WorldHeightMap.cpp
#    Source/W3DDevice/GameLogic/W3DGameLogic.cpp
#    Source/W3DDevice/GameLogic/W3DGhostObject.cpp
#    Source/W3DDevice/GameLogic/W3DTerrainLogic.cpp
    Source/Win32Device/Common/Win32BIGFile.cpp
    Source/Win32Device/Common/Win32BIGFileSystem.cpp
    Source/Win32Device/Common/Win32GameEngine.cpp
    Source/Win32Device/Common/Win32LocalFile.cpp
    Source/Win32Device/Common/Win32LocalFileSystem.cpp
    Source/Win32Device/Common/Win32OSDisplay.cpp
    Source/Win32Device/GameClient/Win32DIKeyboard.cpp
    #Source/Win32Device/GameClient/Win32DIMouse.cpp
    Source/Win32Device/GameClient/Win32Mouse.cpp
)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    list(APPEND GAMEENGINEDEVICE_SRC
        Include/MilesAudioDevice/MilesAudioManager.h
        Include/MilesAudioDevice/MilesAudioCompletionQueue.h
        Include/MilesAudioDevice/AudioChannelPolicy.h
        Source/MilesAudioDevice/MilesAudioManager.cpp
        Source/MilesAudioDevice/AudioChannelPolicy.cpp
    )
endif()

if(NOT RTS_BUILD_OPTION_FFMPEG)
    list(APPEND GAMEENGINEDEVICE_SRC
        Include/VideoDevice/Bink/BinkVideoPlayer.h
        Source/VideoDevice/Bink/BinkVideoPlayer.cpp
    )
endif()

# Add C++ 17 FileSystem implementation for non-VS6 builds
if(NOT IS_VS6_BUILD)
    list(APPEND GAMEENGINEDEVICE_SRC
        Include/StdDevice/Common/StdBIGFile.h
        Include/StdDevice/Common/StdBIGFileSystem.h
        Include/StdDevice/Common/StdLocalFile.h
        Include/StdDevice/Common/StdLocalFileSystem.h
        Source/StdDevice/Common/StdBIGFile.cpp
        Source/StdDevice/Common/StdBIGFileSystem.cpp
        Source/StdDevice/Common/StdLocalFile.cpp
        Source/StdDevice/Common/StdLocalFileSystem.cpp
    )
endif()

add_library(corei_gameenginedevice_private INTERFACE)
add_library(corei_gameenginedevice_public INTERFACE)

target_sources(corei_gameenginedevice_private INTERFACE ${GAMEENGINEDEVICE_SRC})

target_include_directories(corei_gameenginedevice_public INTERFACE
    Include
)

target_link_libraries(corei_gameenginedevice_private INTERFACE
    corei_always
    corei_main
    stb
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_link_libraries(corei_gameenginedevice_private INTERFACE rts_xaudio2)
endif()

target_link_libraries(corei_gameenginedevice_public INTERFACE
    corei_gameengine_public
    rts_legacy_product_runtime
)

if(RTS_BUILD_OPTION_FFMPEG)
    find_package(FFMPEG REQUIRED)

    if(FFMPEG_FOUND)
        add_library(rts_ffmpeg_audio_decoder STATIC
            Include/AudioDevice/AudioPcmTypes.h
            Include/VideoDevice/FFmpeg/FFmpegAudioDecoder.h
            Source/VideoDevice/FFmpeg/FFmpegAudioDecoder.cpp
        )
        target_include_directories(rts_ffmpeg_audio_decoder PUBLIC Include)
        target_link_libraries(rts_ffmpeg_audio_decoder PUBLIC
            FFMPEG::avutil
            FFMPEG::swresample
        )
        target_compile_features(rts_ffmpeg_audio_decoder PUBLIC cxx_std_20)

        target_sources(corei_gameenginedevice_private INTERFACE
            Include/VideoDevice/FFmpeg/FFmpegFile.h
            Include/VideoDevice/FFmpeg/FFmpegVideoPlayer.h
            Source/VideoDevice/FFmpeg/FFmpegFile.cpp
            Source/VideoDevice/FFmpeg/FFmpegVideoPlayer.cpp
        )
        target_link_libraries(corei_gameenginedevice_private INTERFACE
            rts_ffmpeg_audio_decoder
            FFMPEG::avcodec
            FFMPEG::avformat
            FFMPEG::avutil
            FFMPEG::swresample
            FFMPEG::swscale
        )
        target_compile_definitions(corei_gameenginedevice_public INTERFACE RTS_HAS_FFMPEG)
    endif()
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND WIN32)
    add_library(rts_xaudio2_pcm_voice STATIC
        Include/XAudio2AudioDevice/XAudio2PcmVoice.h
        Source/XAudio2AudioDevice/XAudio2PcmVoice.cpp
    )
    target_include_directories(rts_xaudio2_pcm_voice PUBLIC Include)
    target_link_libraries(rts_xaudio2_pcm_voice PUBLIC rts_xaudio2)
    target_compile_features(rts_xaudio2_pcm_voice PUBLIC cxx_std_20)
    if(RTS_BUILD_OPTION_FFMPEG)
        target_link_libraries(rts_xaudio2_pcm_voice PRIVATE rts_ffmpeg_audio_decoder)
        target_compile_definitions(rts_xaudio2_pcm_voice PRIVATE RTS_XAUDIO2_PCM_HAS_FFMPEG)
    endif()
    target_link_libraries(corei_gameenginedevice_private INTERFACE rts_xaudio2_pcm_voice)

    add_library(rts_xaudio2_audio_service STATIC
        Include/XAudio2AudioDevice/IXAudio2AudioEngineBackend.h
        Include/XAudio2AudioDevice/XAudio2AudioService.h
        Include/XAudio2AudioDevice/XAudio2NativeAudioEngine.h
        Source/XAudio2AudioDevice/XAudio2AudioService.cpp
        Source/XAudio2AudioDevice/XAudio2NativeAudioEngine.cpp
    )
    target_include_directories(rts_xaudio2_audio_service PUBLIC Include)
    target_link_libraries(rts_xaudio2_audio_service PUBLIC
        rts_xaudio2
        rts_xaudio2_pcm_voice
    )
    target_compile_features(rts_xaudio2_audio_service PUBLIC cxx_std_20)
    target_link_libraries(corei_gameenginedevice_private INTERFACE rts_xaudio2_audio_service)
endif()
