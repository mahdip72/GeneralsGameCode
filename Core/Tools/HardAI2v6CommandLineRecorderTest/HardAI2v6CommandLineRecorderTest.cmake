# Command & Conquer Generals(tm)
# Copyright 2026 TheSuperHackers
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Native x64 source-connected true8AI2v6 CLI/Recorder contract helper.
# Parent-owned title CMakeLists remain responsible for registration.

include_guard(GLOBAL)

# CMAKE_CURRENT_LIST_DIR inside a function is the caller's list context on
# older supported CMake versions.  Keep the definition directory stable for
# the later target_sources call.
set(_HARD_AI_2V6_TEST_DEFINITION_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(_hard_ai_2v6_extract_block source_variable marker include_semicolon output_variable)
	set(_source "${${source_variable}}")
	string(FIND "${_source}" "${marker}" _start)
	if(_start LESS 0)
		message(FATAL_ERROR "hard-ai-2v6 source marker is missing: ${marker}")
	endif()
	string(SUBSTRING "${_source}" ${_start} -1 _tail)
	string(LENGTH "${marker}" _marker_length)
	string(SUBSTRING "${_tail}" ${_marker_length} -1 _after_marker)
	string(FIND "${_after_marker}" "${marker}" _duplicate)
	if(NOT _duplicate EQUAL -1)
		message(FATAL_ERROR "hard-ai-2v6 source marker is duplicated: ${marker}")
	endif()
	string(FIND "${_tail}" "{" _brace)
	if(_brace LESS 0)
		message(FATAL_ERROR "hard-ai-2v6 source block has no body: ${marker}")
	endif()

	string(LENGTH "${_tail}" _tail_length)
	set(_depth 0)
	set(_index ${_brace})
	set(_block_length 0)
	while(_index LESS _tail_length)
		string(SUBSTRING "${_tail}" ${_index} 1 _char)
		if(_char STREQUAL "{")
			math(EXPR _depth "${_depth} + 1")
		elseif(_char STREQUAL "}")
			math(EXPR _depth "${_depth} - 1")
			if(_depth EQUAL 0)
				math(EXPR _block_length "${_index} + 1")
				break()
			endif()
		endif()
		math(EXPR _index "${_index} + 1")
	endwhile()
	if(_block_length EQUAL 0)
		message(FATAL_ERROR "hard-ai-2v6 source block is unterminated: ${marker}")
	endif()
	if(include_semicolon)
		if(NOT _block_length LESS _tail_length)
			message(FATAL_ERROR "hard-ai-2v6 declaration has no semicolon: ${marker}")
		endif()
		string(SUBSTRING "${_tail}" ${_block_length} 1 _terminator)
		if(NOT _terminator STREQUAL ";")
			message(FATAL_ERROR "hard-ai-2v6 declaration terminator moved: ${marker}")
		endif()
		math(EXPR _block_length "${_block_length} + 1")
	endif()
	string(SUBSTRING "${_tail}" 0 ${_block_length} _block)
	set(${output_variable} "${_block}" PARENT_SCOPE)
endfunction()

function(_hard_ai_2v6_extract_slice source_variable begin_marker end_marker output_variable)
	set(_source "${${source_variable}}")
	string(FIND "${_source}" "${begin_marker}" _begin)
	if(_begin LESS 0)
		message(FATAL_ERROR "hard-ai-2v6 source slice begin is missing: ${begin_marker}")
	endif()
	string(SUBSTRING "${_source}" ${_begin} -1 _tail)
	string(LENGTH "${begin_marker}" _begin_length)
	string(SUBSTRING "${_tail}" ${_begin_length} -1 _after_begin)
	string(FIND "${_after_begin}" "${begin_marker}" _duplicate_begin)
	if(NOT _duplicate_begin EQUAL -1)
		message(FATAL_ERROR "hard-ai-2v6 source slice begin is duplicated: ${begin_marker}")
	endif()
	string(FIND "${_tail}" "${end_marker}" _end)
	if(_end LESS 1)
		message(FATAL_ERROR "hard-ai-2v6 source slice end is missing: ${end_marker}")
	endif()
	string(LENGTH "${end_marker}" _end_length)
	math(EXPR _after_end_start "${_end} + ${_end_length}")
	string(SUBSTRING "${_tail}" ${_after_end_start} -1 _after_end)
	string(FIND "${_after_end}" "${end_marker}" _duplicate_end)
	if(NOT _duplicate_end EQUAL -1)
		message(FATAL_ERROR "hard-ai-2v6 source slice end is duplicated: ${end_marker}")
	endif()
	string(SUBSTRING "${_tail}" 0 ${_end} _slice)
	set(${output_variable} "${_slice}" PARENT_SCOPE)
endfunction()

function(_hard_ai_2v6_extract_until source_variable begin_marker end_marker output_variable)
	set(_source "${${source_variable}}")
	string(FIND "${_source}" "${begin_marker}" _begin)
	if(_begin LESS 0)
		message(FATAL_ERROR "hard-ai-2v6 source prefix is missing: ${begin_marker}")
	endif()
	string(SUBSTRING "${_source}" ${_begin} -1 _tail)
	string(LENGTH "${begin_marker}" _begin_length)
	string(SUBSTRING "${_tail}" ${_begin_length} -1 _after_begin)
	string(FIND "${_after_begin}" "${begin_marker}" _duplicate_begin)
	if(NOT _duplicate_begin EQUAL -1)
		message(FATAL_ERROR "hard-ai-2v6 source prefix is duplicated: ${begin_marker}")
	endif()
	string(FIND "${_tail}" "${end_marker}" _end)
	if(_end LESS 1)
		message(FATAL_ERROR "hard-ai-2v6 source prefix terminator is missing: ${end_marker}")
	endif()
	string(LENGTH "${end_marker}" _end_length)
	math(EXPR _slice_length "${_end} + ${_end_length}")
	string(SUBSTRING "${_tail}" 0 ${_slice_length} _slice)
	set(${output_variable} "${_slice}" PARENT_SCOPE)
endfunction()

function(_hard_ai_2v6_append_callback_if_present source_variable callback output_variable present_output)
	set(_source "${${source_variable}}")
	string(FIND "${_source}" "Int ${callback}(" _start)
	if(_start LESS 0)
		set(${output_variable} "${${output_variable}}" PARENT_SCOPE)
		set(${present_output} FALSE PARENT_SCOPE)
		return()
	endif()
	_hard_ai_2v6_extract_block(${source_variable} "Int ${callback}(" FALSE _block)
	set(_value "${${output_variable}}")
	string(APPEND _value "\n${_block}\n")
	set(${output_variable} "${_value}" PARENT_SCOPE)
	set(${present_output} TRUE PARENT_SCOPE)
endfunction()

function(_hard_ai_2v6_add_title_test target title test_name)
	set(_command_path "${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/Common/CommandLine.cpp")
	set(_runner_path "${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/Common/SkirmishAITestRunner.cpp")
	set(_scenario_header_path "${CMAKE_SOURCE_DIR}/Core/GameEngine/Include/Common/SkirmishAITestRunner.h")
	set(_data_path "${CMAKE_SOURCE_DIR}/${title}/Code/GameEngine/Include/Common/GlobalData.h")
	set(_game_main_path "${CMAKE_SOURCE_DIR}/${title}/Code/GameEngine/Source/Common/GameMain.cpp")
	set(_recorder_path "${CMAKE_SOURCE_DIR}/${title}/Code/GameEngine/Source/Common/Recorder.cpp")
	set(_trim_path "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WWLib/trim.cpp")
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
		"${_command_path}" "${_runner_path}" "${_scenario_header_path}" "${_data_path}"
		"${_game_main_path}" "${_recorder_path}" "${_trim_path}")

	file(READ "${_command_path}" _command)
	file(READ "${_runner_path}" _runner)
	file(READ "${_scenario_header_path}" _scenario_header)
	file(READ "${_data_path}" _data)
	file(READ "${_game_main_path}" _game_main)
	file(READ "${_recorder_path}" _recorder)
	file(READ "${_trim_path}" _trim)
	foreach(_text_variable IN ITEMS _command _runner _scenario_header _data _game_main _recorder _trim)
		string(REPLACE "\r\n" "\n" ${_text_variable} "${${_text_variable}}")
	endforeach()

	# Keep the actual title request methods and the actual scenario values.  The
	# reviewed hard request accessors are a prerequisite for compiling this test;
	# their absence is setup-blocked, not the behavioral RED below.
	_hard_ai_2v6_extract_block(_data "class CommandLineData" TRUE _data_class)
	_hard_ai_2v6_extract_block(_scenario_header "enum SkirmishAITestScenario" TRUE _scenario_enum)

	_hard_ai_2v6_extract_until(_command
		"typedef Int (*FuncPtr)( char *args[], int num );"
		";" _command_source)
	# Keep only the typedef itself; the prefix helper includes its exact
	# terminator and does not consume the following struct declaration.
	_hard_ai_2v6_extract_block(_command "struct CommandLineParam" TRUE _param_struct)
	string(APPEND _command_source "\n${_param_struct}\n")
	_hard_ai_2v6_extract_block(_trim "char* strtrim(char* buffer)" FALSE _trim_block)
	string(APPEND _command_source "\n${_trim_block}\n")

	# The callbacks whose behavior this test observes are exact source bodies.
	# All other table callbacks are generated as throwing dependency stubs below.
	set(_connected_callbacks
		parseWin parseNoWin parseHeadless parseReplay parseWorkerCount
		parseWorkerPolicy parseNoLogo parseRunSkirmishAITest
		parseRunSkirmishAITest4v2 parseRunSkirmishAITestPractical1v7
		parseRunSkirmishAITestHardAI2v6 parseRunSkirmishAITestForStartup
		parseRunSkirmishAITest4v2ForStartup
		parseRunSkirmishAITestPractical1v7ForStartup
		parseRunSkirmishAITestHardAI2v6ForStartup)
	# The extracted callbacks call this exact helper, so retain source order:
	# TryParse -> parseSkirmishAITestSeedArgument -> parseRun callbacks.
	_hard_ai_2v6_extract_block(_runner "Bool TryParseSkirmishAITestSeed(" FALSE _seed_parser)
	string(APPEND _command_source "\n${_seed_parser}\n")
	_hard_ai_2v6_extract_block(_command
		"Bool parseSkirmishAITestSeedArgument(" FALSE _seed_argument)
	string(APPEND _command_source "\n${_seed_argument}\n")
	set(_connected_present_callbacks)
	foreach(_callback IN LISTS _connected_callbacks)
		_hard_ai_2v6_append_callback_if_present(_command "${_callback}"
			_command_source _present)
		if(_present)
			list(APPEND _connected_present_callbacks "${_callback}")
		endif()
	endforeach()
	_hard_ai_2v6_extract_block(_command "static CommandLineParam paramsForStartup[] =" TRUE _startup_table)
	_hard_ai_2v6_extract_block(_command "static CommandLineParam paramsForEngineInit[] =" TRUE _init_table)

	# Make omitted production callbacks fail loudly if an exercised command
	# accidentally enters an unrelated lane.  Callback names are read from the
	# complete real tables, so this list does not duplicate a production table.
	string(REGEX MATCHALL "\\{[ \\t\\r\\n]*\"-[^\"]+\"[ \\t\\r\\n]*,[ \\t\\r\\n]*[A-Za-z_][A-Za-z0-9_]*[ \\t\\r\\n]*\\}" _callback_rows
		"${_startup_table}\n${_init_table}")
	set(_stub_source "")
	set(_stubbed_callbacks)
	foreach(_row IN LISTS _callback_rows)
		string(REGEX REPLACE ".*,[ \\t\\r\\n]*([A-Za-z_][A-Za-z0-9_]*)[ \\t\\r\\n]*\\}" "\\1" _callback "${_row}")
		if(_callback IN_LIST _connected_present_callbacks OR _callback IN_LIST _stubbed_callbacks)
			continue()
		endif()
		string(APPEND _stub_source
			"Int ${_callback}(char *[], int) { throw UnexpectedCallback(\"${_callback}\"); }\n")
		list(APPEND _stubbed_callbacks "${_callback}")
	endforeach()
	string(APPEND _command_source "\n${_stub_source}\n${_startup_table}\n${_init_table}\n")

	foreach(_marker IN ITEMS
			"char *nextParam(char *newSource, const char *seps)"
			"static void parseCommandLine(const CommandLineParam* params, int numParams)"
			"void createGlobalData()"
			"void CommandLine::parseCommandLineForStartup()"
			"void CommandLine::parseCommandLineForEngineInit()")
		_hard_ai_2v6_extract_block(_command "${_marker}" FALSE _block)
		string(APPEND _command_source "\n${_block}\n")
	endforeach()

	# This is exactly the GameMain branch that maps a request to the scenario;
	# no test-side arm ladder is permitted.
	_hard_ai_2v6_extract_block(_game_main
		"if (!net3ValidationRequested && !lockstepV2ValidationRequested)"
		FALSE _game_main_arm)

	# Reuse the same source-slice boundaries as the existing Generals recorder
	# epoch fixture, but add only the local-index writer and read/apply blocks.
	_hard_ai_2v6_extract_slice(_recorder "// Write the slot list."
		"\n\t/*\n\t/// @todo fix this" _recorder_writer)
	_hard_ai_2v6_extract_slice(_recorder "char *playerIndexEnd = nullptr;"
		"\n\t// Read in the GameInfo." _recorder_reader)
	_hard_ai_2v6_extract_slice(_recorder
		"if (header.localPlayerIndex >= 0 && header.localPlayerIndex < MAX_SLOTS)"
		"\n\tif (!header.forPlayback)" _recorder_apply)

	set(_prefix "HardAI2v6${title}")
	file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${_prefix}CommandLineData.inc"
		CONTENT "${_data_class}\n")
	file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${_prefix}ScenarioEnum.inc"
		CONTENT "${_scenario_enum}\n")
	file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${_prefix}CommandLine.inc"
		CONTENT "${_command_source}\n")
	file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${_prefix}GameMainArm.inc"
		CONTENT "${_game_main_arm}\n")
	file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${_prefix}RecorderWriter.inc"
		CONTENT "${_recorder_writer}\n")
	file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${_prefix}RecorderReader.inc"
		CONTENT "${_recorder_reader}\n")
	file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${_prefix}RecorderApply.inc"
		CONTENT "${_recorder_apply}\n")

	target_sources(${target} PRIVATE
		"${_HARD_AI_2V6_TEST_DEFINITION_DIR}/HardAI2v6CommandLineRecorderTest.cpp")
	target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
	if(title STREQUAL "Generals")
		target_compile_definitions(${target} PRIVATE HARD_AI2V6_GENERALS)
	else()
		target_compile_definitions(${target} PRIVATE HARD_AI2V6_ZERO_HOUR)
	endif()
	target_compile_definitions(${target} PRIVATE
		HARD_AI2V6_GLOBAL_DATA_COMMAND_LINE_DATA_INC="${_prefix}CommandLineData.inc"
		HARD_AI2V6_SCENARIO_ENUM_INC="${_prefix}ScenarioEnum.inc"
		HARD_AI2V6_COMMAND_LINE_SOURCE_INC="${_prefix}CommandLine.inc"
		HARD_AI2V6_GAME_MAIN_ARM_SOURCE_INC="${_prefix}GameMainArm.inc"
		HARD_AI2V6_RECORDER_WRITER_SOURCE_INC="${_prefix}RecorderWriter.inc"
		HARD_AI2V6_RECORDER_READER_PARSE_SOURCE_INC="${_prefix}RecorderReader.inc"
		HARD_AI2V6_RECORDER_READER_APPLY_SOURCE_INC="${_prefix}RecorderApply.inc")
	add_test(NAME "${test_name}" COMMAND ${target} --hard-ai-2v6-cli-recorder)
	set_tests_properties("${test_name}" PROPERTIES TIMEOUT 30)
endfunction()
