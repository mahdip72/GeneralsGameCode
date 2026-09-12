set(_ai_owner_source
    "${CMAKE_SOURCE_DIR}/GeneralsMD/Code/GameEngine/Source/GameLogic/AI/AISkirmishPlayer.cpp")
set(_ai_owner_prefix_include
    "${CMAKE_CURRENT_BINARY_DIR}/AIOwnerProductionPrefix.inc")
if(NOT EXISTS "${_ai_owner_source}")
    message(FATAL_ERROR
        "Zero Hour owner source is required for the source-connected AI planning regression: ${_ai_owner_source}")
endif()
file(READ "${_ai_owner_source}" _ai_owner_source_text)
string(FIND "${_ai_owner_source_text}"
    "Bool AISkirmishPlayer::selectTeamToBuildCounterSerialFallback("
    _ai_owner_function_begin)
if(_ai_owner_function_begin LESS 0)
    message(FATAL_ERROR
        "Could not find the Zero Hour owner-planning function in ${_ai_owner_source}")
endif()
string(SUBSTRING "${_ai_owner_source_text}" ${_ai_owner_function_begin} -1
    _ai_owner_function_suffix)
string(FIND "${_ai_owner_function_suffix}"
    "rts::RecordAIPlanningOwnerCapture((UnsignedInt)candidateFacts.size());"
    _ai_owner_prefix_relative_begin)
if(_ai_owner_prefix_relative_begin LESS 0)
    message(FATAL_ERROR
        "Could not find the Zero Hour owner-planning prefix start in ${_ai_owner_source}")
endif()
math(EXPR _ai_owner_prefix_begin
    "${_ai_owner_function_begin} + ${_ai_owner_prefix_relative_begin}")
string(SUBSTRING "${_ai_owner_source_text}" ${_ai_owner_prefix_begin} -1
    _ai_owner_prefix_suffix)
string(FIND "${_ai_owner_prefix_suffix}"
    "TeamPrototype *selectedPrototype = NULL;" _ai_owner_prefix_end)
if(_ai_owner_prefix_end LESS 0)
    message(FATAL_ERROR
        "Could not find the Zero Hour owner-planning prefix continuation in ${_ai_owner_source}")
endif()
string(SUBSTRING "${_ai_owner_prefix_suffix}" 0 ${_ai_owner_prefix_end}
    _ai_owner_prefix)
string(FIND "${_ai_owner_prefix}" "!result.valid" _ai_owner_invalid_guard)
string(FIND "${_ai_owner_prefix}"
    "rts::RecordAIPlanningOwnerCommit(false);" _ai_owner_invalid_rejection)
if(_ai_owner_invalid_guard LESS 0 OR _ai_owner_invalid_rejection LESS 0)
    message(FATAL_ERROR
        "Zero Hour owner-planning prefix no longer preserves invalid-result rejection")
endif()
file(GENERATE OUTPUT "${_ai_owner_prefix_include}"
    CONTENT "${_ai_owner_prefix}\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_ai_owner_source}")

add_executable(core_deterministic_ai_planning_tests
	DeterministicAIPlanningTest.cpp
    "${_ai_owner_prefix_include}")
target_include_directories(core_deterministic_ai_planning_tests PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(core_deterministic_ai_planning_tests PRIVATE core_task_runtime)
set_target_properties(core_deterministic_ai_planning_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
if(MINGW)
    target_link_options(core_deterministic_ai_planning_tests PRIVATE -mconsole)
endif()

add_test(NAME core_deterministic_ai_planning_tests
    COMMAND core_deterministic_ai_planning_tests --local-capacity)
set_tests_properties(core_deterministic_ai_planning_tests PROPERTIES TIMEOUT 30)
