# CPU-only regression using production merge blocks and actual WWMath headers.
set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/Generated")
set(generated_inputs "${generated_dir}/segline.inc" "${generated_dir}/streak.inc")
set(line_source "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WW3D2")
add_custom_command(
    OUTPUT ${generated_inputs} "${generated_dir}/extraction-receipt.json"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/ExtractMerge.ps1"
        -SourceRoot "${CMAKE_SOURCE_DIR}" -OutputRoot "${generated_dir}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ExtractMerge.ps1"
        "${line_source}/seglinerenderer.cpp" "${line_source}/streakRender.cpp"
    VERBATIM)
add_custom_target(core_native_line_merge_extract DEPENDS
    ${generated_inputs} "${generated_dir}/extraction-receipt.json")
add_executable(core_native_line_merge_tests NativeLineMergeTest.cpp ${generated_inputs})
set_source_files_properties(${generated_inputs} PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE)
add_dependencies(core_native_line_merge_tests core_native_line_merge_extract)
target_include_directories(core_native_line_merge_tests PRIVATE
    "${generated_dir}" "${CMAKE_SOURCE_DIR}/Core/Libraries/Source"
    "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas"
    "${CMAKE_SOURCE_DIR}/Core/Libraries/Include" "${CMAKE_SOURCE_DIR}/Dependencies/Utility")
target_compile_definitions(core_native_line_merge_tests PRIVATE DISABLE_GAMEMEMORY)
set_target_properties(core_native_line_merge_tests PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
add_test(NAME core_native_line_merge_tests COMMAND core_native_line_merge_tests)
set_tests_properties(core_native_line_merge_tests PROPERTIES
    TIMEOUT 30 LABELS "renderer;native;geometry;line-merge")
