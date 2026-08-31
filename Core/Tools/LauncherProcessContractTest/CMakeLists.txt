add_executable(core_launcher_process_contract_test
    LauncherProcessContractTest.cpp
    ../Launcher/process.cpp
)

target_compile_features(core_launcher_process_contract_test PRIVATE cxx_std_20)

target_compile_definitions(core_launcher_process_contract_test PRIVATE
    RTS_LAUNCHER_PROCESS_ONLY
)

target_include_directories(core_launcher_process_contract_test PRIVATE
    ../Launcher
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
    ${CMAKE_SOURCE_DIR}/Dependencies/Utility
)

add_test(NAME core_launcher_process_contract_test
    COMMAND core_launcher_process_contract_test
)

set_tests_properties(core_launcher_process_contract_test PROPERTIES TIMEOUT 15)
