set(COMPRESSION_SRC
    Compression.h
    CompressionManager.cpp
    EAC/btreeabout.cpp
    EAC/btreecodex.h
    EAC/btreedecode.cpp
    EAC/btreeencode.cpp
    EAC/codex.h
    EAC/gimex.h
    EAC/huffabout.cpp
    EAC/huffcodex.h
    EAC/huffdecode.cpp
    EAC/huffencode.cpp
    EAC/refabout.cpp
    EAC/refcodex.h
    EAC/refdecode.cpp
    EAC/refencode.cpp
    LZHCompress/NoxCompress.cpp
    LZHCompress/NoxCompress.h
)

add_library(core_compression STATIC)
set_target_properties(core_compression PROPERTIES OUTPUT_NAME compression)

target_sources(core_compression PRIVATE ${COMPRESSION_SRC})

target_include_directories(core_compression INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(core_compression PRIVATE
    corei_libraries_include
    corei_always
)


target_link_libraries(core_compression PUBLIC
    liblzhl
)

find_package(ZLIB)
if (ZLIB_FOUND)
    # Adds zlib from vcpkg
    target_link_libraries(core_compression PUBLIC ZLIB::ZLIB)

    # Standard Windows vcpkg triplets link zlib dynamically. Resolve only the
    # exact per-configuration DLLs so native installed products remain
    # app-local and clean-machine runnable.
    set(RTS_NATIVE_ZLIB_RUNTIME_DLLS_RELEASE)
    set(RTS_NATIVE_ZLIB_RUNTIME_DLLS_DEBUG)
    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND
            VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
        foreach(_RTS_ZLIB_CONFIG IN ITEMS RELEASE DEBUG)
            if(_RTS_ZLIB_CONFIG STREQUAL "RELEASE")
                set(_RTS_ZLIB_RUNTIME_DIRECTORY
                    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
            else()
                set(_RTS_ZLIB_RUNTIME_DIRECTORY
                    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin")
            endif()
            file(GLOB _RTS_ZLIB_RUNTIME_CANDIDATES LIST_DIRECTORIES FALSE
                "${_RTS_ZLIB_RUNTIME_DIRECTORY}/zlib*.dll")
            list(LENGTH _RTS_ZLIB_RUNTIME_CANDIDATES _RTS_ZLIB_RUNTIME_COUNT)
            if(_RTS_ZLIB_RUNTIME_COUNT GREATER 1)
                message(FATAL_ERROR
                    "Expected at most one ${_RTS_ZLIB_CONFIG} vcpkg zlib runtime in ${_RTS_ZLIB_RUNTIME_DIRECTORY}.")
            elseif(_RTS_ZLIB_RUNTIME_COUNT EQUAL 0 AND
                    VCPKG_TARGET_TRIPLET STREQUAL "x64-windows")
                message(FATAL_ERROR
                    "The dynamic x64-windows triplet did not provide a ${_RTS_ZLIB_CONFIG} zlib runtime in ${_RTS_ZLIB_RUNTIME_DIRECTORY}.")
            elseif(_RTS_ZLIB_RUNTIME_COUNT EQUAL 1)
                set(RTS_NATIVE_ZLIB_RUNTIME_DLLS_${_RTS_ZLIB_CONFIG}
                    "${_RTS_ZLIB_RUNTIME_CANDIDATES}" CACHE INTERNAL
                    "Resolved ${_RTS_ZLIB_CONFIG} vcpkg zlib runtime" FORCE)
            endif()
        endforeach()
        unset(_RTS_ZLIB_CONFIG)
        unset(_RTS_ZLIB_RUNTIME_CANDIDATES)
        unset(_RTS_ZLIB_RUNTIME_COUNT)
        unset(_RTS_ZLIB_RUNTIME_DIRECTORY)
    endif()
else()
    # Adds zlib from github repository
    include(${CMAKE_SOURCE_DIR}/cmake/zlib.cmake)
    target_link_libraries(core_compression PUBLIC libzlib)
endif()
