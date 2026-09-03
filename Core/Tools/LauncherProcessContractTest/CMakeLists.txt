set(_launcher_command_line_source
    "${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/Common/CommandLine.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_launcher_command_line_source}")
file(READ "${_launcher_command_line_source}" _launcher_command_line_contents)
string(REPLACE "\r\n" "\n" _launcher_command_line_contents
    "${_launcher_command_line_contents}")

foreach(_launcher_option_entry IN ITEMS
        "{ \"-workerPolicy\", parseWorkerPolicy }"
        "{ \"-simulationMode\", parseSimulationMode }")
    string(FIND "${_launcher_command_line_contents}" "${_launcher_option_entry}"
        _launcher_option_entry_offset)
    if(_launcher_option_entry_offset LESS 0)
        message(FATAL_ERROR
            "The production startup option entry is missing: ${_launcher_option_entry}")
    endif()
endforeach()

string(FIND "${_launcher_command_line_contents}"
    "Int parseWorkerPolicy(char *args[], int num)" _launcher_worker_parser_start)
string(FIND "${_launcher_command_line_contents}"
    "Int parsePipelineMode(char *args[], int num)" _launcher_worker_parser_end)
if(_launcher_worker_parser_start LESS 0 OR _launcher_worker_parser_end LESS 0 OR
        _launcher_worker_parser_end LESS _launcher_worker_parser_start)
    message(FATAL_ERROR "The production worker-policy parser markers are missing.")
endif()
math(EXPR _launcher_worker_parser_length
    "${_launcher_worker_parser_end} - ${_launcher_worker_parser_start}")
string(SUBSTRING "${_launcher_command_line_contents}"
    ${_launcher_worker_parser_start} ${_launcher_worker_parser_length}
    _launcher_worker_parser)

string(FIND "${_launcher_command_line_contents}"
    "Int parseSimulationMode(char *args[], int num)" _launcher_simulation_parser_start)
string(FIND "${_launcher_command_line_contents}"
    "Int parseValidationExecutableSha256(char *args[], int num)"
    _launcher_simulation_parser_start_end)
if(_launcher_simulation_parser_start LESS 0 OR
        _launcher_simulation_parser_start_end LESS 0 OR
        _launcher_simulation_parser_start_end LESS _launcher_simulation_parser_start)
    message(FATAL_ERROR "The production simulation-mode parser markers are missing.")
endif()
math(EXPR _launcher_simulation_parser_length
    "${_launcher_simulation_parser_start_end} - ${_launcher_simulation_parser_start}")
string(SUBSTRING "${_launcher_command_line_contents}"
    ${_launcher_simulation_parser_start} ${_launcher_simulation_parser_length}
    _launcher_simulation_parser)

string(FIND "${_launcher_command_line_contents}"
    "Int parseXRes(char *args[], int num)" _launcher_validation_parser_end)
if(_launcher_validation_parser_end LESS 0 OR
        _launcher_validation_parser_end LESS _launcher_simulation_parser_start_end)
    message(FATAL_ERROR "The production validation-hash parser markers are missing.")
endif()
math(EXPR _launcher_validation_parser_length
    "${_launcher_validation_parser_end} - ${_launcher_simulation_parser_start_end}")
string(SUBSTRING "${_launcher_command_line_contents}"
    ${_launcher_simulation_parser_start_end} ${_launcher_validation_parser_length}
    _launcher_validation_parser)

set(_launcher_parsers_header
    "${CMAKE_CURRENT_BINARY_DIR}/LauncherGameStartupParsers.h")
file(WRITE "${_launcher_parsers_header}"
    "${_launcher_worker_parser}\n${_launcher_simulation_parser}${_launcher_validation_parser}")

# Generate the same LCF that the native product packaging helper emits.  The
# resulting file is passed to the test so the 32-bit lane exercises the helper's
# pointer-size-4 branch instead of a handwritten legacy string.
add_executable(core_launcher_lcf_contract_fixture IMPORTED GLOBAL)
set_property(TARGET core_launcher_lcf_contract_fixture PROPERTY IMPORTED_LOCATION
    "${CMAKE_CURRENT_BINARY_DIR}/launcher-contract-target.exe")
set(_launcher_contract_lcf
    "${CMAKE_CURRENT_BINARY_DIR}/launcher-contract.lcf")
rts_generate_launcher_lcf(core_launcher_lcf_contract_fixture
    "${_launcher_contract_lcf}")

add_executable(core_launcher_process_contract_test
    LauncherProcessContractTest.cpp
    ../Launcher/configfile.cpp
    ../Launcher/process.cpp
    ../Launcher/wstring.cpp
)

target_compile_features(core_launcher_process_contract_test PRIVATE cxx_std_20)
target_compile_definitions(core_launcher_process_contract_test PRIVATE
    RTS_LAUNCHER_POINTER_BYTES=${CMAKE_SIZEOF_VOID_P})

target_link_libraries(core_launcher_process_contract_test PRIVATE core_task_runtime)

target_include_directories(core_launcher_process_contract_test PRIVATE
    ../Launcher
    "${CMAKE_CURRENT_BINARY_DIR}"
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
    ${CMAKE_SOURCE_DIR}/Dependencies/Utility
)

add_test(NAME core_launcher_process_contract_test
    COMMAND core_launcher_process_contract_test "${_launcher_contract_lcf}"
)

set_tests_properties(core_launcher_process_contract_test PROPERTIES TIMEOUT 15)
