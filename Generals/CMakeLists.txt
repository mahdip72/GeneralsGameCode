cmake_minimum_required(VERSION 3.25)

project(g_generals LANGUAGES C CXX)

if (RTS_BUILD_GENERALS_DOCS)
    find_package(Doxygen REQUIRED)
    doxygen_add_docs(g_docs Code)
endif()

# Set where the build results will end up
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# Add main build targets
add_subdirectory(Code)

# Keep registry discovery isolated from the user-controlled install prefix.
# Only the legacy Win32 path may use the registry value as a fallback.
set(_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS "${RTS_INSTALL_PREFIX_GENERALS}")
if("${CMAKE_HOST_SYSTEM}" MATCHES "Windows" AND "${CMAKE_SYSTEM}" MATCHES "Windows"
        AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    include(${CMAKE_SOURCE_DIR}/cmake/registry.cmake)

    # Check the EA App/CD registry path
    fetch_registry_value(
        "HKEY_LOCAL_MACHINE/SOFTWARE/Electronic Arts/EA Games/Generals"
        "InstallPath"
        RTS_REGISTRY_INSTALL_PREFIX_GENERALS
        "Generals install path from registry")

    # Check the "First Decade" registry path
    fetch_registry_value(
        "HKEY_LOCAL_MACHINE/SOFTWARE/Electronic Arts/EA Games/Command and Conquer The First Decade"
        "gr_folder"
        RTS_REGISTRY_INSTALL_PREFIX_GENERALS
        "Generals install path from registry")

    # Check the Steam registry path
    fetch_registry_value(
        "HKEY_LOCAL_MACHINE/SOFTWARE/Electronic Arts/EA Games/Generals"
        "installPath"
        RTS_REGISTRY_INSTALL_PREFIX_GENERALS
        "Generals install path from registry")

    if(NOT RTS_INSTALL_PREFIX_GENERALS)
        set(_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS
            "${RTS_REGISTRY_INSTALL_PREFIX_GENERALS}")
    endif()
endif()

if(_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS)
    install(TARGETS g_generals RUNTIME DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS}")
    install(FILES $<TARGET_PDB_FILE:g_generals> DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS}" OPTIONAL)

    if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND TARGET g_launcher)
        install(TARGETS g_launcher RUNTIME DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS}")
        install(FILES $<TARGET_PDB_FILE:g_launcher> DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS}" OPTIONAL)
    endif()

    if(TARGET g_worldbuilder)
        install(TARGETS g_worldbuilder RUNTIME DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS}")
        install(FILES $<TARGET_PDB_FILE:g_worldbuilder> DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS}" OPTIONAL)
    endif()

    if(TARGET core_particleeditor)
        install(TARGETS core_particleeditor RUNTIME DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS}")
        install(FILES $<TARGET_PDB_FILE:core_particleeditor> DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS}" OPTIONAL)
    endif()

    if(TARGET core_debugwindow)
        install(TARGETS core_debugwindow RUNTIME DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS}")
        install(FILES $<TARGET_PDB_FILE:core_debugwindow> DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS}" OPTIONAL)
    endif()

    if(TARGET g_guiedit)
        install(TARGETS g_guiedit RUNTIME DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS}")
        install(FILES $<TARGET_PDB_FILE:g_guiedit> DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS}" OPTIONAL)
    endif()

    if(TARGET g_imagepacker)
        install(TARGETS g_imagepacker RUNTIME DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS}")
        install(FILES $<TARGET_PDB_FILE:g_imagepacker> DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS}" OPTIONAL)
    endif()

    if(TARGET g_mapcachebuilder)
        install(TARGETS g_mapcachebuilder RUNTIME DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS}")
        install(FILES $<TARGET_PDB_FILE:g_mapcachebuilder> DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS}" OPTIONAL)
    endif()

    if(TARGET g_w3dview)
        install(TARGETS g_w3dview RUNTIME DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS}")
        install(FILES $<TARGET_PDB_FILE:g_w3dview> DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS}" OPTIONAL)
    endif()
else()
    message(STATUS "Generals install path not found. Registry keys for 'Generals' not found. "
                   "Generals targets will be built, but not installed. "
                   "You can specify the path manually by setting RTS_INSTALL_PREFIX_GENERALS.")
endif()
