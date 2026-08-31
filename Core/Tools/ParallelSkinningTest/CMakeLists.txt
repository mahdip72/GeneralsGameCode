add_executable(core_parallel_skinning_portable_tests ParallelSkinningPortableTest.cpp)
target_link_libraries(core_parallel_skinning_portable_tests PRIVATE core_task_runtime)
set(_skinning_test_targets core_parallel_skinning_portable_tests)

if(WIN32)
    # The production WWMath oracle pulls the Windows-only WWLib PCH. Keep the
    # separate POD fixture above available for device-free Unix validation.
    add_executable(core_parallel_skinning_tests ParallelSkinningTest.cpp)
    target_include_directories(core_parallel_skinning_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas
    )
    target_link_libraries(core_parallel_skinning_tests PRIVATE
        core_task_runtime
        core_wwmath
        core_wwlib
        core_wwdebug
        core_wwstub
        corei_always
        winmm
    )
    list(APPEND _skinning_test_targets core_parallel_skinning_tests)
endif()

foreach(_skinning_target IN LISTS _skinning_test_targets)
    if(RTS_BUILD_CORE_EXTRAS)
        target_compile_definitions(${_skinning_target} PRIVATE RTS_BUILD_CORE_EXTRAS)
    endif()
    if(IS_VS6_BUILD)
        set_target_properties(${_skinning_target} PROPERTIES CXX_STANDARD 98)
    else()
        set_target_properties(${_skinning_target} PROPERTIES CXX_STANDARD 20)
    endif()
    set_target_properties(${_skinning_target} PROPERTIES
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
    if(MINGW)
        target_link_options(${_skinning_target} PRIVATE -mconsole)
    endif()
    add_test(NAME ${_skinning_target} COMMAND ${_skinning_target})
    set_tests_properties(${_skinning_target} PROPERTIES TIMEOUT 90)
endforeach()
