# CPU-only control fixture: actual pass header + shared water methods and both
# title admission fragments. Owner execution is covered by ResourcesTest.
set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/Generated")
set(generated_inputs "${generated_dir}/water.inc"
    "${generated_dir}/Generals-display.inc" "${generated_dir}/GeneralsMD-display.inc")
add_custom_command(OUTPUT ${generated_inputs} "${generated_dir}/extraction-receipt.json"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/ExtractMethods.ps1"
        -SourceRoot "${CMAKE_SOURCE_DIR}" -OutputRoot "${generated_dir}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ExtractMethods.ps1"
        "${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp"
        "${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Include/W3DDevice/GameClient/W3DWater.h"
        "${CMAKE_SOURCE_DIR}/Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp"
        "${CMAKE_SOURCE_DIR}/GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp"
    VERBATIM)
add_custom_target(core_native_sorted_texture_pass_extract DEPENDS
    ${generated_inputs} "${generated_dir}/extraction-receipt.json")
add_executable(core_native_sorted_texture_pass_tests
    NativeSortedTexturePassTest.cpp ${generated_inputs})
set_source_files_properties(${generated_inputs} PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE)
add_dependencies(core_native_sorted_texture_pass_tests core_native_sorted_texture_pass_extract)
target_include_directories(core_native_sorted_texture_pass_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/Core/Libraries/Include"
    "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas"
    "${CMAKE_SOURCE_DIR}/Dependencies/Utility" "${generated_dir}")
target_link_libraries(core_native_sorted_texture_pass_tests PRIVATE core_utility_no_pch)
set_target_properties(core_native_sorted_texture_pass_tests PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
add_test(NAME core_native_sorted_texture_pass_tests COMMAND core_native_sorted_texture_pass_tests)
set_tests_properties(core_native_sorted_texture_pass_tests PROPERTIES TIMEOUT 30 LABELS "cpu;native-sorted-pass")
