# Native64 CPU-only: actual MD line geometry, shared facade/handler and sorter.
set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/Generated")
set(generated_inputs
    "${generated_dir}/linegroup.inc"
    "${generated_dir}/command-helpers.inc"
    "${generated_dir}/command-facade.inc"
    "${generated_dir}/owner-sorted.inc")
set(native_source "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WW3D2")
set(title_source "${CMAKE_SOURCE_DIR}/GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2")
add_custom_command(
    OUTPUT ${generated_inputs} "${generated_dir}/extraction-receipt.json"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/ExtractMethods.ps1"
        -SourceRoot "${CMAKE_SOURCE_DIR}" -OutputRoot "${generated_dir}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/ExtractMethods.ps1"
        "${title_source}/linegrp.cpp" "${title_source}/linegrp.h"
        "${native_source}/nativew3dgameclientcommands.cpp"
        "${native_source}/nativew3d2.cpp"
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Include/Renderer/RenderGameClient.h"
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Include/Renderer/RenderGameClientNative.h"
    VERBATIM)
add_custom_target(core_native_linegroup_sorting_extract DEPENDS
    ${generated_inputs} "${generated_dir}/extraction-receipt.json")
add_executable(core_native_linegroup_sorting_tests
    NativeLineGroupSortingTest.cpp "${native_source}/nativew3dsorting.cpp"
    ${generated_inputs})
set_source_files_properties(${generated_inputs} PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE)
add_dependencies(core_native_linegroup_sorting_tests core_native_linegroup_sorting_extract)
target_include_directories(core_native_linegroup_sorting_tests PRIVATE
    "${native_source}" "${generated_dir}")
target_link_libraries(core_native_linegroup_sorting_tests PRIVATE
    core_renderer core_task_runtime core_utility_no_pch)
set_target_properties(core_native_linegroup_sorting_tests PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
add_test(NAME core_native_linegroup_sorting_tests COMMAND core_native_linegroup_sorting_tests)
set_tests_properties(core_native_linegroup_sorting_tests PROPERTIES
    TIMEOUT 90 LABELS "renderer;native;geometry;sorting")
