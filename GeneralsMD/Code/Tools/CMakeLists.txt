# Build tools

# Build useful tool binaries.
if(RTS_BUILD_ZEROHOUR_TOOLS)
    add_subdirectory(GUIEdit)
    add_subdirectory(ImagePacker)
    add_subdirectory(MapCacheBuilder)
    add_subdirectory(W3DView)
    add_subdirectory(wdump)
    add_subdirectory(WorldBuilder)
endif()

# Build less useful tool/test binaries.
if(RTS_BUILD_ZEROHOUR_EXTRAS AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_subdirectory(Autorun)
    add_subdirectory(PATCHGET)
endif()

# The native x64 product always carries the runtime contract utility. Historical
# Win32 authoring graphs may build it only when their extras are requested.
if(RTS_BUILD_ZEROHOUR_EXTRAS OR (CMAKE_SIZEOF_VOID_P EQUAL 8 AND RTS_BUILD_PRODUCT))
    add_subdirectory(RuntimeRegressionTests)
endif()

# Native x64 product builds always need the launcher in their installed graph.
# Historical Win32 authoring graphs get it only with their extras.
if(RTS_BUILD_ZEROHOUR_EXTRAS OR (CMAKE_SIZEOF_VOID_P EQUAL 8 AND RTS_BUILD_PRODUCT))
    add_subdirectory(Launcher)
endif()
