if(NOT RTS_BUILD_PRODUCT)
    # The native product lane owns the one x64 resource registry.  Core-only
    # graphs do not need this product-bound integration fixture.
    return()
endif()

add_executable(core_native_w3d_buffer_owner_tests
    NativeW3DBufferOwnerTest.cpp
)

target_include_directories(core_native_w3d_buffer_owner_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas"
    "${CMAKE_SOURCE_DIR}/Dependencies/Utility"
)

target_precompile_headers(core_native_w3d_buffer_owner_tests PRIVATE
    [["Utility/CppMacros.h"]]
)

target_link_libraries(core_native_w3d_buffer_owner_tests PRIVATE
    core_ww3d2_native
    core_wwstub
    core_wwlib
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
