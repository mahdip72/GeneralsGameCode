add_executable(core_native_xaudio_manager_contract_test
	${CMAKE_SOURCE_DIR}/Core/Tools/NativeAudioManagerContractTest/NativeAudioManagerContractTest.cpp
	${CMAKE_SOURCE_DIR}/Core/Tools/NativeAudioManagerContractTest/NativeAudioManagerRuntimeSupport.cpp
	${CMAKE_SOURCE_DIR}/Core/Tools/NativeAudioAssetCatalogTest/NativeAudioAssetCatalogRuntimeSupport.cpp
)

target_compile_definitions(core_native_xaudio_manager_contract_test PRIVATE
	DISABLE_GAMEMEMORY
	RTS_NATIVE_AUDIO_TEST_HOOK
)

target_include_directories(core_native_xaudio_manager_contract_test PRIVATE
	${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Include
	${CMAKE_SOURCE_DIR}/Core/GameEngine/Include
	${CMAKE_SOURCE_DIR}/Core/GameEngine/Include/Precompiled
	${CMAKE_SOURCE_DIR}/Core/Libraries/Include
	${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas
	${CMAKE_SOURCE_DIR}/Core/Main
	${CMAKE_SOURCE_DIR}/Dependencies/Utility
)

target_compile_options(core_native_xaudio_manager_contract_test PRIVATE
	/FI${CMAKE_SOURCE_DIR}/Dependencies/Utility/Utility/CppMacros.h
)

target_link_libraries(core_native_xaudio_manager_contract_test PRIVATE
	rts_native_audio_manager
	rts_xaudio2_audio_service
	rts_native_audio_asset_source
	core_wwlib
	core_wwdebug
	core_wwstub
	winmm
)

set_target_properties(core_native_xaudio_manager_contract_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_native_xaudio_manager_contract_test
    COMMAND core_native_xaudio_manager_contract_test
)
set_tests_properties(core_native_xaudio_manager_contract_test PROPERTIES TIMEOUT 30)
if(WIN32 AND RTS_BUILD_OPTION_FFMPEG AND FFMPEG_RUNTIME_DIR)
    set_tests_properties(core_native_xaudio_manager_contract_test PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${FFMPEG_RUNTIME_DIR}"
    )
endif()
