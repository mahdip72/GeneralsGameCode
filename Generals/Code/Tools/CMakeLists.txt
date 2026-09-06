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

# Native x64 product builds always need the launcher in their installed graph.
# Historical Win32 authoring graphs get it only with their extras.
if(RTS_BUILD_GENERALS_EXTRAS OR (CMAKE_SIZEOF_VOID_P EQUAL 8 AND RTS_BUILD_PRODUCT))
    add_subdirectory(Launcher)
endif()

# Exercise the shared native runtime contracts through the Generals title
# graph as well as Zero Hour. The utility remains outside historical Win32
# authoring graphs unless their extras are enabled.
if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND
        (RTS_BUILD_GENERALS_EXTRAS OR RTS_BUILD_PRODUCT))
    add_subdirectory(RuntimeRegressionTests)
    add_subdirectory(DeterministicAIPlanningTest)
endif()
