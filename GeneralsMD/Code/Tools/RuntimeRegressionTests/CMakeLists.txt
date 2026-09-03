add_library(z_xcrc OBJECT
    "${CMAKE_SOURCE_DIR}/Core/Tools/XferCrcSnapshotTest/XferCrcSnapshotTest.cpp"
)
target_include_directories(z_xcrc PRIVATE
    "${CMAKE_SOURCE_DIR}/Core/Tools/XferCrcSnapshotTest"
)
target_link_libraries(z_xcrc PRIVATE
    z_gameengine
    zi_always
)
set_target_properties(z_xcrc PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)

add_executable(z_runtime_regression_tests)

target_sources(z_runtime_regression_tests PRIVATE
    RuntimeRegressionTests.cpp
    $<TARGET_OBJECTS:z_xcrc>
)

target_include_directories(z_runtime_regression_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/Core/Tools/XferCrcSnapshotTest"
)

# Exercise the same architecture-selected product boundary as the title
# executable. Win32/VC6 resolves this to the legacy D3D8/Miles/Bink lane;
# native x64 resolves it to the D3D11/XAudio2 boundary.
target_link_libraries(z_runtime_regression_tests PRIVATE
	comctl32
	core_debug
	core_profile_legacy
	imm32
	rts_product_runtime
	vfw32
	winmm
	z_gameengine
	z_gameenginedevice
	zi_always
)

add_test(NAME z_skirmish_ai_replay_epoch_tests COMMAND z_runtime_regression_tests --skirmish-ai-replay-epoch)
set_tests_properties(z_skirmish_ai_replay_epoch_tests PROPERTIES TIMEOUT 30)

add_test(NAME z_skirmish_ai_runner_contract_tests COMMAND z_runtime_regression_tests --skirmish-ai-runner-contract)
set_tests_properties(z_skirmish_ai_runner_contract_tests PROPERTIES TIMEOUT 30)

add_test(NAME z_xfer_crc_snapshot_tests
    COMMAND z_runtime_regression_tests --xfer-crc-snapshot)
set_tests_properties(z_xfer_crc_snapshot_tests PROPERTIES TIMEOUT 30)

if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND WIN32 AND NOT IS_VS6_BUILD)
	include("${CMAKE_SOURCE_DIR}/Core/Tools/Stage5PerformanceFixtureTest/HeadlessMetricStartupTitleTest.cmake")
	rts_add_headless_metric_startup_test(z_runtime_regression_tests
		GeneralsMD z_headless_metric_startup_tests)
	add_executable(z_native_render_target_texture_tests
		"${CMAKE_SOURCE_DIR}/Core/Tools/NativeW3DTextureOwnerTest/NativeRenderTargetTextureTest.cpp"
	)
	target_include_directories(z_native_render_target_texture_tests PRIVATE
		"${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WW3D2"
	)
	target_link_libraries(z_native_render_target_texture_tests PRIVATE
		core_wwstub
		core_ww3d2_native
		z_ww3d2
		vfw32
		winmm
	)
	set_target_properties(z_native_render_target_texture_tests PROPERTIES
		CXX_STANDARD 20
		CXX_STANDARD_REQUIRED YES
		CXX_EXTENSIONS NO
	)
	add_test(NAME z_native_render_target_texture_tests
		COMMAND z_native_render_target_texture_tests)
	set_tests_properties(z_native_render_target_texture_tests PROPERTIES TIMEOUT 30)

	add_executable(z_native_line3d_render_tests
		"${CMAKE_SOURCE_DIR}/Core/Tools/NativeW3DLineTest/line3dprod.cpp"
	)
	target_include_directories(z_native_line3d_render_tests PRIVATE
		"${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas"
		"${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WW3D2"
		"${CMAKE_SOURCE_DIR}/GeneralsMD/Code/Libraries/Source/WWVegas"
		"${CMAKE_SOURCE_DIR}/GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2"
	)
	target_link_libraries(z_native_line3d_render_tests PRIVATE
		core_wwstub
		core_ww3d2_native
		z_ww3d2
		vfw32
		winmm
	)
	set_target_properties(z_native_line3d_render_tests PROPERTIES
		CXX_STANDARD 20
		CXX_STANDARD_REQUIRED YES
		CXX_EXTENSIONS NO
	)
	add_test(NAME z_native_line3d_render_tests
		COMMAND z_native_line3d_render_tests)
	set_tests_properties(z_native_line3d_render_tests PROPERTIES TIMEOUT 60)

	# Exercise the real product AudioEventRTS/GameAudio implementations, not the
	# isolated manager fixture's runtime substitutes. The hook only sets capacity.
	target_link_libraries(z_runtime_regression_tests PRIVATE rts_native_audio_manager)
	target_compile_definitions(z_runtime_regression_tests PRIVATE RTS_NATIVE_AUDIO_TEST_HOOK)
	if(RTS_BUILD_OPTION_FFMPEG AND RTS_FFMPEG_RUNTIME_DLLS)
		# This executable now imports FFmpeg in every mode. Stage the exact
		# validated closure (including swresample/swscale), as the product install
		# already does, so direct build-tree invocations are app-local as well.
		add_custom_command(TARGET z_runtime_regression_tests POST_BUILD
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				${RTS_FFMPEG_RUNTIME_DLLS} "$<TARGET_FILE_DIR:z_runtime_regression_tests>"
			COMMAND_EXPAND_LISTS
			VERBATIM
		)
	endif()
	add_test(NAME z_native_logical_audio_tests
		COMMAND z_runtime_regression_tests --native-logical-audio)
	set_tests_properties(z_native_logical_audio_tests PROPERTIES TIMEOUT 30)

	# Capture the target's full CMake link closure at configure time.  A direct
	# LINK_LIBRARIES property is insufficient because a legacy dependency can
	# arrive through an INTERFACE target several levels below the executable.
	# The audit also examines the generated linker command after the target is
	# built, so this file is a diagnostic input rather than the sole authority.
	set_property(GLOBAL PROPERTY RTS_RUNTIME_REGRESSION_CLOSURE_SEEN "")
	set_property(GLOBAL PROPERTY RTS_RUNTIME_REGRESSION_CLOSURE_LINES
		"target=z_runtime_regression_tests")

	function(rts_collect_runtime_regression_link_closure target_name)
		if(NOT TARGET "${target_name}")
			return()
		endif()

		get_property(seen_targets GLOBAL PROPERTY RTS_RUNTIME_REGRESSION_CLOSURE_SEEN)
		if("${target_name}" IN_LIST seen_targets)
			return()
		endif()
		set_property(GLOBAL APPEND PROPERTY RTS_RUNTIME_REGRESSION_CLOSURE_SEEN
			"${target_name}")

		get_property(closure_lines GLOBAL PROPERTY RTS_RUNTIME_REGRESSION_CLOSURE_LINES)
		list(APPEND closure_lines "dependency=${target_name}")
		set_property(GLOBAL PROPERTY RTS_RUNTIME_REGRESSION_CLOSURE_LINES
			"${closure_lines}")

		foreach(link_property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
			get_target_property(link_items "${target_name}" "${link_property}")
			if(NOT link_items OR link_items STREQUAL "link_items-NOTFOUND")
				continue()
			endif()

			foreach(link_item IN LISTS link_items)
				if("${link_item}" STREQUAL "")
					continue()
				endif()

				get_property(closure_lines GLOBAL PROPERTY RTS_RUNTIME_REGRESSION_CLOSURE_LINES)
				list(APPEND closure_lines "dependency=${link_item}")
				set_property(GLOBAL PROPERTY RTS_RUNTIME_REGRESSION_CLOSURE_LINES
					"${closure_lines}")

				# Follow the common CMake wrappers so transitive interface libraries
				# are represented.  Unresolved configuration expressions remain in
				# the diagnostic file and are checked by the generated linker command.
				set(candidate "${link_item}")
				if(candidate MATCHES "^\\$<LINK_ONLY:(.*)>$")
					set(candidate "${CMAKE_MATCH_1}")
				elseif(candidate MATCHES "^\\$<BUILD_INTERFACE:(.*)>$")
					set(candidate "${CMAKE_MATCH_1}")
				elseif(candidate MATCHES "^\\$<TARGET_NAME_IF_EXISTS:([^>]+)>$")
					set(candidate "${CMAKE_MATCH_1}")
				endif()

				if(TARGET "${candidate}")
					rts_collect_runtime_regression_link_closure("${candidate}")
				endif()
			endforeach()
		endforeach()
	endfunction()

	rts_collect_runtime_regression_link_closure(z_runtime_regression_tests)
	get_property(runtime_regression_closure GLOBAL PROPERTY
		RTS_RUNTIME_REGRESSION_CLOSURE_LINES)
	list(JOIN runtime_regression_closure "\n" runtime_regression_closure_text)
	file(WRITE "${CMAKE_BINARY_DIR}/z_runtime_regression_link_closure.txt"
		"${runtime_regression_closure_text}\n")

	add_test(NAME z_runtime_regression_graph_audit
		COMMAND powershell -NoProfile -ExecutionPolicy Bypass
			-File "${CMAKE_CURRENT_SOURCE_DIR}/RuntimeRegressionTestsGraphAudit.ps1"
			-SourceRoot "${CMAKE_SOURCE_DIR}"
			-BuildRoot "${CMAKE_BINARY_DIR}"
	)
	set_tests_properties(z_runtime_regression_graph_audit PROPERTIES TIMEOUT 240)

	add_test(NAME z_runtime_regression_graph_audit_self_test
		COMMAND powershell -NoProfile -ExecutionPolicy Bypass
			-File "${CMAKE_CURRENT_SOURCE_DIR}/RuntimeRegressionTestsGraphAudit.ps1"
			-SelfTest
	)
	set_tests_properties(z_runtime_regression_graph_audit_self_test PROPERTIES TIMEOUT 30)
endif()

add_test(NAME z_texture_load_queue_contract_tests COMMAND z_runtime_regression_tests --texture-load-queue-contract)
set_tests_properties(z_texture_load_queue_contract_tests PROPERTIES TIMEOUT 30)

if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND WIN32 AND NOT IS_VS6_BUILD
		AND RTS_BUILD_OPTION_FFMPEG AND FFMPEG_RUNTIME_DIR)
	# All four tests share the same loader dependency set, not just the audio
	# mode. Keep the SDK lookup local to these test processes.
	set_tests_properties(
		z_skirmish_ai_replay_epoch_tests
		z_skirmish_ai_runner_contract_tests
		z_native_logical_audio_tests
		z_texture_load_queue_contract_tests
		PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${FFMPEG_RUNTIME_DIR}"
	)
endif()
