add_library(core_native_audio_asset_catalog_compile OBJECT
    NativeAudioAssetCatalogTest.cpp
)

target_include_directories(core_native_audio_asset_catalog_compile PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Include
    ${CMAKE_SOURCE_DIR}/Core/GameEngine/Include
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
    ${CMAKE_SOURCE_DIR}/Dependencies/Utility
)

target_compile_options(core_native_audio_asset_catalog_compile PRIVATE
    /FI${CMAKE_SOURCE_DIR}/Dependencies/Utility/Utility/CppMacros.h
)

set_target_properties(core_native_audio_asset_catalog_compile PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_executable(core_native_audio_asset_catalog_test
	NativeAudioAssetCatalogTest.cpp
)

target_compile_definitions(core_native_audio_asset_catalog_test PRIVATE DISABLE_GAMEMEMORY)
if(RTS_BUILD_OPTION_FFMPEG AND FFMPEG_EXECUTABLE)
    target_compile_definitions(core_native_audio_asset_catalog_test PRIVATE
        RTS_NATIVE_AUDIO_HAS_FFMPEG_CLI)
endif()

target_include_directories(core_native_audio_asset_catalog_test PRIVATE
	${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Include
	${CMAKE_SOURCE_DIR}/Core/GameEngine/Include
	${CMAKE_SOURCE_DIR}/Core/GameEngine/Include/Precompiled
	${CMAKE_SOURCE_DIR}/Core/Libraries/Include
	${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas
	${CMAKE_SOURCE_DIR}/Dependencies/Utility
)

target_sources(core_native_audio_asset_catalog_test PRIVATE
	NativeAudioAssetCatalogRuntimeSupport.cpp
)

target_link_libraries(core_native_audio_asset_catalog_test PRIVATE
    rts_native_audio_asset_source
	core_wwlib
	core_wwdebug
	core_wwstub
	winmm
)

target_compile_options(core_native_audio_asset_catalog_test PRIVATE
	/FI${CMAKE_SOURCE_DIR}/Dependencies/Utility/Utility/CppMacros.h
)

set_target_properties(core_native_audio_asset_catalog_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

if(RTS_BUILD_OPTION_FFMPEG AND FFMPEG_EXECUTABLE)
    add_test(NAME core_native_audio_asset_catalog_test
        COMMAND core_native_audio_asset_catalog_test
            --ffmpeg "${FFMPEG_EXECUTABLE}"
    )
else()
    add_test(NAME core_native_audio_asset_catalog_test
        COMMAND core_native_audio_asset_catalog_test
    )
endif()
set_tests_properties(core_native_audio_asset_catalog_test PROPERTIES TIMEOUT 30)
if(WIN32 AND RTS_BUILD_OPTION_FFMPEG AND FFMPEG_RUNTIME_DIR)
    set_tests_properties(core_native_audio_asset_catalog_test PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${FFMPEG_RUNTIME_DIR}"
    )
endif()
