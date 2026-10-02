# CPU-only actual-method regression; no title library, GPU or baseline Git ref.
function(add_native_material_override_test target_name title)
    set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/Generated/${title}")
    set(generated_inputs
        "${generated_dir}/category.inc"
        "${generated_dir}/contracts.inc"
        "${generated_dir}/material-facade.inc"
        "${generated_dir}/material-values.inc"
        "${generated_dir}/mapper.inc"
        "${generated_dir}/owner-material.inc")
    set(title_source "${CMAKE_SOURCE_DIR}/${title}/Code/Libraries/Source/WWVegas/WW3D2")
    add_custom_command(
        OUTPUT ${generated_inputs} "${generated_dir}/extraction-receipt.json"
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/ExtractMethods.ps1"
            -SourceRoot "${CMAKE_SOURCE_DIR}"
            -OutputRoot "${generated_dir}"
            -Title "${title}"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/ExtractMethods.ps1"
            "${CMAKE_SOURCE_DIR}/Core/Libraries/Include/Renderer/RenderGameClient.h"
            "${CMAKE_SOURCE_DIR}/Core/Libraries/Include/Renderer/RenderGameClientNative.h"
            "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WW3D2/nativew3dmeshrenderer.cpp"
            "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WW3D2/nativew3d2.cpp"
            "${title_source}/RenderGameClientNativeTitle.cpp"
            "${title_source}/mapper.cpp"
            "${title_source}/vertmaterial.cpp"
        VERBATIM)
    add_custom_target(${target_name}_extract DEPENDS
        ${generated_inputs} "${generated_dir}/extraction-receipt.json")
    add_executable(${target_name}
        NativeMaterialOverrideTest.cpp
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/Renderer/LegacyRenderState.cpp"
        ${generated_inputs})
    set_source_files_properties(${generated_inputs} PROPERTIES
        GENERATED TRUE HEADER_FILE_ONLY TRUE)
    add_dependencies(${target_name} ${target_name}_extract)
    target_include_directories(${target_name} PRIVATE
        "${CMAKE_SOURCE_DIR}/Core/Libraries/Include" "${generated_dir}")
    if(title STREQUAL "Generals")
        target_compile_definitions(${target_name} PRIVATE FIXTURE_GENERALS=1)
    endif()
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    add_test(NAME ${target_name} COMMAND ${target_name})
    set_tests_properties(${target_name} PROPERTIES TIMEOUT 30)
endfunction()

# Both source contracts exist even in a single-title product configuration.
add_native_material_override_test(g_native_material_override_tests Generals)
add_native_material_override_test(z_native_material_override_tests GeneralsMD)
