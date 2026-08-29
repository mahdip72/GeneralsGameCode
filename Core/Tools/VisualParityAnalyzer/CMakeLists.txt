find_package(Python3 COMPONENTS Interpreter QUIET)

if(Python3_Interpreter_FOUND)
    add_test(NAME core_visual_parity_analyzer_tests
        COMMAND ${Python3_EXECUTABLE}
            -m unittest discover
            -s "${CMAKE_CURRENT_SOURCE_DIR}/tests"
            -p "test_*.py"
            -v
    )
    set_tests_properties(core_visual_parity_analyzer_tests PROPERTIES
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
endif()
