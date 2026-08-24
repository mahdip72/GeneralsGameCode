# Build tools

# Build useful tool binaries.
if(RTS_BUILD_GENERALS_TOOLS)
    add_subdirectory(GUIEdit)
    add_subdirectory(ImagePacker)
    add_subdirectory(MapCacheBuilder)
    add_subdirectory(W3DView)
    add_subdirectory(WorldBuilder)
endif()

# Build less useful tool/test binaries.
if(RTS_BUILD_GENERALS_EXTRAS AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_subdirectory(Autorun)
    add_subdirectory(PATCHGET)
endif()

# Native x64 product builds always need the launcher in their installed
# product graph.  Keep the legacy Win32 option behavior: Win32 gets it only
# when the extras preset is selected.
if(RTS_BUILD_GENERALS_EXTRAS OR (CMAKE_SIZEOF_VOID_P EQUAL 8 AND RTS_BUILD_PRODUCT))
    add_subdirectory(Launcher)
endif()
