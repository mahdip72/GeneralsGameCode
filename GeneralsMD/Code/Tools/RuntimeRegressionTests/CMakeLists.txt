add_executable(z_runtime_regression_tests)

target_sources(z_runtime_regression_tests PRIVATE
    RuntimeRegressionTests.cpp
)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
	target_link_libraries(z_runtime_regression_tests PRIVATE
		comctl32
		core_debug
		core_profile_legacy
		d3d8
		d3dx8
		dinput8
		dxguid
		imm32
		milesstub
		vfw32
		winmm
		z_gameengine
		z_gameenginedevice
		zi_always
	)

	if(NOT RTS_BUILD_OPTION_FFMPEG)
		target_link_libraries(z_runtime_regression_tests PRIVATE binkstub)
	endif()
else()
	# The native x64 utility exercises logic/network contracts only.  Keep it
	# on the product's modern dependency boundary and do not pull the legacy
	# D3D8, Miles, or Bink import libraries into the test executable.
	#
	# rts_d3d8_headers is intentionally allowed in this staging graph: it is a
	# header-only compatibility boundary for the temporary parity sources, not
	# a D3D8 import/library dependency.  The graph audit rejects d3d8, d3dx8,
	# rts_d3d8lib, Miles, and Bink while explicitly permitting that interface.
	target_link_libraries(z_runtime_regression_tests PRIVATE
		comctl32
		core_debug
		core_profile_legacy
		imm32
		rts_legacy_product_runtime
		vfw32
		winmm
		z_gameengine
		z_gameenginedevice
		zi_always
	)
endif()

add_test(NAME z_skirmish_ai_replay_epoch_tests COMMAND z_runtime_regression_tests --skirmish-ai-replay-epoch)
set_tests_properties(z_skirmish_ai_replay_epoch_tests PROPERTIES TIMEOUT 30)

add_test(NAME z_skirmish_ai_runner_contract_tests COMMAND z_runtime_regression_tests --skirmish-ai-runner-contract)
set_tests_properties(z_skirmish_ai_runner_contract_tests PROPERTIES TIMEOUT 30)

if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND WIN32 AND NOT IS_VS6_BUILD)
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
