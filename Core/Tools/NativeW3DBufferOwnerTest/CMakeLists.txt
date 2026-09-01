if(NOT RTS_BUILD_PRODUCT OR NOT TARGET rts_d3d8_headers)
    # This fixture intentionally exercises the product's complete WW3D
    # implementation, not only the neutral owner library. Device-free/core-only
    # graphs have no title renderer to link and therefore do not register it.
    return()
endif()

add_executable(core_native_w3d_buffer_owner_tests
    NativeW3DBufferOwnerTest.cpp
)

if(RTS_BUILD_ZEROHOUR)
    set(_rts_native_buffer_owner_title_wwvegas
        "${CMAKE_SOURCE_DIR}/GeneralsMD/Code/Libraries/Source/WWVegas")
    set(_rts_native_buffer_owner_full_renderer z_ww3d2)
elseif(RTS_BUILD_GENERALS)
    set(_rts_native_buffer_owner_title_wwvegas
        "${CMAKE_SOURCE_DIR}/Generals/Code/Libraries/Source/WWVegas")
    set(_rts_native_buffer_owner_full_renderer g_ww3d2)
else()
    message(FATAL_ERROR
        "NativeW3DBufferOwnerTest requires a selected product renderer.")
endif()

target_include_directories(core_native_w3d_buffer_owner_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas"
    "${_rts_native_buffer_owner_title_wwvegas}"
    "${_rts_native_buffer_owner_title_wwvegas}/WW3D2"
    "${CMAKE_SOURCE_DIR}/Dependencies/Utility"
)

target_precompile_headers(core_native_w3d_buffer_owner_tests PRIVATE
    [["Utility/CppMacros.h"]]
)

target_link_libraries(core_native_w3d_buffer_owner_tests PRIVATE
    core_ww3d2_native
    core_wwstub
    rts_d3d8_headers
    ${_rts_native_buffer_owner_full_renderer}
    vfw32
    winmm
)

set_target_properties(core_native_w3d_buffer_owner_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_native_w3d_buffer_owner_tests
    COMMAND core_native_w3d_buffer_owner_tests
)
set_tests_properties(core_native_w3d_buffer_owner_tests PROPERTIES TIMEOUT 30)
