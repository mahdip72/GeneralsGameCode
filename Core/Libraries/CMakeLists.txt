# WW common libraries decended from earliest C&C games
add_subdirectory(Source/WWVegas)

# profiling library
add_subdirectory(Source/profile)

# debugging library
add_subdirectory(Source/debug)

add_subdirectory(Source/EABrowserDispatch)
add_subdirectory(Source/EABrowserEngine)
add_subdirectory(Source/Compression)
add_subdirectory(Source/TaskRuntime)
add_subdirectory(Source/Renderer)
if(NOT IS_VS6_BUILD)
    add_subdirectory(Source/RuntimeEpoch)
    add_subdirectory(Source/ReplayCommand)
endif()
