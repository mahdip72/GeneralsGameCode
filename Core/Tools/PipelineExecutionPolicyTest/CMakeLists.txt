set(_pipeline_command_line_source "${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/Common/CommandLine.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_pipeline_command_line_source}")
file(READ "${_pipeline_command_line_source}" _pipeline_command_line_contents)
string(REPLACE "\r\n" "\n" _pipeline_command_line_contents "${_pipeline_command_line_contents}")
string(FIND "${_pipeline_command_line_contents}" "Int parsePipelineMode(char *args[], int num)" _pipeline_parser_start)
if(_pipeline_parser_start LESS 0)
    message(FATAL_ERROR "Pipeline mode parser start marker is missing")
endif()
string(SUBSTRING "${_pipeline_command_line_contents}" ${_pipeline_parser_start} -1 _pipeline_parser)
string(FIND "${_pipeline_parser}" "Int parseRunSkirmishAITestPractical1v7(char *args[], int num)" _pipeline_parser_end)
if(_pipeline_parser_end LESS 0)
    message(FATAL_ERROR "Pipeline mode parser adjacent end marker is missing")
endif()
string(SUBSTRING "${_pipeline_parser}" 0 ${_pipeline_parser_end} _pipeline_parser)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/PipelineModeParserUnderTest.h" "${_pipeline_parser}")

add_executable(core_pipeline_execution_policy_tests PipelineExecutionPolicyTest.cpp)
target_include_directories(core_pipeline_execution_policy_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(core_pipeline_execution_policy_tests PRIVATE core_task_runtime)
if(IS_VS6_BUILD)
    set_target_properties(core_pipeline_execution_policy_tests PROPERTIES CXX_STANDARD 98)
else()
    set_target_properties(core_pipeline_execution_policy_tests PROPERTIES CXX_STANDARD 20)
endif()
set_target_properties(core_pipeline_execution_policy_tests PROPERTIES
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
if(MINGW)
    target_link_options(core_pipeline_execution_policy_tests PRIVATE -mconsole)
endif()

add_test(NAME core_pipeline_execution_policy_parser_test
    COMMAND core_pipeline_execution_policy_tests parser "${_pipeline_command_line_source}")
add_test(NAME core_pipeline_execution_policy_owner_test COMMAND core_pipeline_execution_policy_tests owner)
add_test(NAME core_pipeline_execution_policy_compute_test COMMAND core_pipeline_execution_policy_tests compute)
set_tests_properties(
    core_pipeline_execution_policy_parser_test
    core_pipeline_execution_policy_owner_test
    core_pipeline_execution_policy_compute_test
    PROPERTIES TIMEOUT 30
)
if(NOT IS_VS6_BUILD)
    add_test(NAME core_pipeline_execution_policy_race_test COMMAND core_pipeline_execution_policy_tests race)
    set_tests_properties(core_pipeline_execution_policy_race_test PROPERTIES TIMEOUT 30)
endif()
