add_executable(core_native_product_device_lifecycle_tests
    NativeProductDeviceLifecycleTest.cpp
)

target_include_directories(core_native_product_device_lifecycle_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Include
)

set_target_properties(core_native_product_device_lifecycle_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_native_product_device_lifecycle_tests
    COMMAND core_native_product_device_lifecycle_tests
)
set_tests_properties(core_native_product_device_lifecycle_tests PROPERTIES TIMEOUT 30)
