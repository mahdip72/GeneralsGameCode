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

target_compile_options(core_native_audio_asset_catalog_test PRIVATE
	/FI${CMAKE_SOURCE_DIR}/Dependencies/Utility/Utility/CppMacros.h
)

set_target_properties(core_native_audio_asset_catalog_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_native_audio_asset_catalog_test
    COMMAND core_native_audio_asset_catalog_test
)
set_tests_properties(core_native_audio_asset_catalog_test PROPERTIES TIMEOUT 30)
