# Architecture-selected product dependency boundary. Product consumers link
# only this target; architecture-specific dependencies remain behind it.
include_guard(GLOBAL)

add_library(rts_product_runtime INTERFACE)

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    include("${CMAKE_CURRENT_LIST_DIR}/native-product-runtime.cmake")
    target_link_libraries(rts_product_runtime INTERFACE
        rts_native_product_runtime
    )
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
    include("${CMAKE_CURRENT_LIST_DIR}/legacy-product-runtime.cmake")
    target_link_libraries(rts_product_runtime INTERFACE
        rts_legacy_product_runtime
    )
elseif(RTS_BUILD_PRODUCT)
    message(FATAL_ERROR
        "Product executables support only 32-bit legacy builds or native x64 Windows builds.")
else()
    # Non-product host/tool graphs may use their platform-specific stubs.
    set_property(TARGET rts_product_runtime PROPERTY
        INTERFACE_RTS_PRODUCT_RUNTIME_SELECTION none)
endif()

file(GENERATE
    OUTPUT "${CMAKE_BINARY_DIR}/product_runtime_selection.txt"
    CONTENT "target=rts_product_runtime\nlinks=$<JOIN:$<TARGET_PROPERTY:rts_product_runtime,INTERFACE_LINK_LIBRARIES>,|>\n"
)
