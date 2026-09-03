add_executable(core_native_sorting_renderer_tests
    NativeSortingRendererTest.cpp
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WW3D2/nativew3dsorting.cpp
)

target_include_directories(core_native_sorting_renderer_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WW3D2
)

target_link_libraries(core_native_sorting_renderer_tests PRIVATE
    core_renderer
    core_task_runtime
    core_utility_no_pch
)

set_target_properties(core_native_sorting_renderer_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

if(MINGW)
    target_link_options(core_native_sorting_renderer_tests PRIVATE -mconsole)
endif()

add_test(NAME core_native_sorting_renderer_tests
    COMMAND core_native_sorting_renderer_tests)
set_tests_properties(core_native_sorting_renderer_tests PROPERTIES TIMEOUT 90)
