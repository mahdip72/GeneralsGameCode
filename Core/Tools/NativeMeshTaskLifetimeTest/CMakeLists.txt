# CPU-only execution of the actual shared category traversal/task lifetime code.
set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/Generated")
set(mesh_source "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WW3D2/nativew3dmeshrenderer.cpp")
set(generated_inputs "${generated_dir}/mesh-task-class.inc" "${generated_dir}/mesh-task-methods.inc")
add_custom_command(OUTPUT ${generated_inputs} "${generated_dir}/extraction-receipt.json"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/ExtractMethods.ps1"
        -SourceRoot "${CMAKE_SOURCE_DIR}" -OutputRoot "${generated_dir}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ExtractMethods.ps1" "${mesh_source}" VERBATIM)
add_custom_target(core_native_mesh_task_lifetime_extract DEPENDS ${generated_inputs} "${generated_dir}/extraction-receipt.json")
add_executable(core_native_mesh_task_lifetime_tests NativeMeshTaskLifetimeTest.cpp ${generated_inputs})
set_source_files_properties(${generated_inputs} PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE)
add_dependencies(core_native_mesh_task_lifetime_tests core_native_mesh_task_lifetime_extract)
target_include_directories(core_native_mesh_task_lifetime_tests PRIVATE "${generated_dir}")
set_target_properties(core_native_mesh_task_lifetime_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
add_test(NAME core_native_mesh_task_lifetime_tests COMMAND core_native_mesh_task_lifetime_tests)
set_tests_properties(core_native_mesh_task_lifetime_tests PROPERTIES TIMEOUT 30 LABELS "renderer;native;task-lifetime")
