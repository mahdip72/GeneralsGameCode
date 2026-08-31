add_executable(core_persist_pointer_token_tests
    PersistPointerTokenTest.cpp
)

target_include_directories(core_persist_pointer_token_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas"
    "${CMAKE_SOURCE_DIR}/Dependencies/Utility"
)

if(WIN32)
    target_include_directories(core_persist_pointer_token_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Include"
    )

    target_link_libraries(core_persist_pointer_token_tests PRIVATE
        core_wwsaveload
        core_wwlib
        core_wwdebug
        winmm
    )

    # Exercise the production template with its release assertions disabled.
    target_compile_definitions(core_persist_pointer_token_tests PRIVATE
        DISABLE_DEBUG_CRASHING
        NDEBUG
    )
endif()

set_target_properties(core_persist_pointer_token_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_persist_pointer_token_tests
    COMMAND core_persist_pointer_token_tests
)
set_tests_properties(core_persist_pointer_token_tests PROPERTIES TIMEOUT 30)
