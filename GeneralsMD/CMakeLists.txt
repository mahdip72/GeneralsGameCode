cmake_minimum_required(VERSION 3.25)

project(z_generals LANGUAGES C CXX)

if (RTS_BUILD_ZEROHOUR_DOCS)
    find_package(Doxygen REQUIRED)
    doxygen_add_docs(z_docs Code)
endif()

# Set where the build results will end up
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# Add main build targets
add_subdirectory(Code)

# Keep registry discovery isolated from the user-controlled install prefix.
# Only the legacy Win32 path may use the registry value as a fallback.
set(_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR "${RTS_INSTALL_PREFIX_ZEROHOUR}")
if("${CMAKE_HOST_SYSTEM}" MATCHES "Windows" AND "${CMAKE_SYSTEM}" MATCHES "Windows"
        AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    include(${CMAKE_SOURCE_DIR}/cmake/registry.cmake)

    # Check the EA App/CD registry path
    fetch_registry_value(
        "HKEY_LOCAL_MACHINE/SOFTWARE/Electronic Arts/EA Games/Command and Conquer Generals Zero Hour"
        "InstallPath"
        RTS_REGISTRY_INSTALL_PREFIX_ZEROHOUR
        "Zero Hour install path from registry")

    # Check the "First Decade" registry path
    fetch_registry_value(
        "HKEY_LOCAL_MACHINE/SOFTWARE/Electronic Arts/EA Games/Command and Conquer The First Decade"
        "zh_folder"
        RTS_REGISTRY_INSTALL_PREFIX_ZEROHOUR
        "Zero Hour install path from registry")

    # Check the Steam registry path
    fetch_registry_value(
        "HKEY_LOCAL_MACHINE/SOFTWARE/Electronic Arts/EA Games/ZeroHour"
        "installPath"
        RTS_REGISTRY_INSTALL_PREFIX_ZEROHOUR
        "Zero Hour install path from registry")

    if(NOT RTS_INSTALL_PREFIX_ZEROHOUR)
        set(_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR
            "${RTS_REGISTRY_INSTALL_PREFIX_ZEROHOUR}")
    endif()
endif()

if(_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR)
    install(TARGETS z_generals RUNTIME DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR}")
    install(FILES $<TARGET_PDB_FILE:z_generals> DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR}" OPTIONAL)

    if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND TARGET z_launcher)
        install(TARGETS z_launcher RUNTIME DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR}")
        install(FILES $<TARGET_PDB_FILE:z_launcher> DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR}" OPTIONAL)
    endif()

    # Keep the native runtime-test utility with the disposable x64 product
    # install.  It is a test-only target and must not change the legacy Win32
    # install contents.
    if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND TARGET z_runtime_regression_tests)
        install(TARGETS z_runtime_regression_tests RUNTIME DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR}")
        install(FILES $<TARGET_PDB_FILE:z_runtime_regression_tests> DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR}" OPTIONAL)
    endif()

    if(TARGET z_worldbuilder)
        install(TARGETS z_worldbuilder RUNTIME DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR}")
        install(FILES $<TARGET_PDB_FILE:z_worldbuilder> DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR}" OPTIONAL)
    endif()

    if(TARGET core_particleeditor)
        install(TARGETS core_particleeditor RUNTIME DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR}")
        install(FILES $<TARGET_PDB_FILE:core_particleeditor> DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR}" OPTIONAL)
    endif()

    if(TARGET core_debugwindow)
        install(TARGETS core_debugwindow RUNTIME DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR}")
        install(FILES $<TARGET_PDB_FILE:core_debugwindow> DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR}" OPTIONAL)
    endif()

    if(TARGET z_guiedit)
        install(TARGETS z_guiedit RUNTIME DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR}")
        install(FILES $<TARGET_PDB_FILE:z_guiedit> DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR}" OPTIONAL)
    endif()

    if(TARGET z_imagepacker)
        install(TARGETS z_imagepacker RUNTIME DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR}")
        install(FILES $<TARGET_PDB_FILE:z_imagepacker> DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR}" OPTIONAL)
    endif()

    if(TARGET z_mapcachebuilder)
        install(TARGETS z_mapcachebuilder RUNTIME DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR}")
        install(FILES $<TARGET_PDB_FILE:z_mapcachebuilder> DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR}" OPTIONAL)
    endif()

    if(TARGET z_w3dview)
        install(TARGETS z_w3dview RUNTIME DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR}")
        install(FILES $<TARGET_PDB_FILE:z_w3dview> DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR}" OPTIONAL)
    endif()

    if(TARGET z_wdump)
        install(TARGETS z_wdump RUNTIME DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR}")
        install(FILES $<TARGET_PDB_FILE:z_wdump> DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR}" OPTIONAL)
    endif()
else()
    message(STATUS "Zero Hour install path not found. Registry keys for 'Command and Conquer Generals Zero Hour' not found. "
                   "Zero Hour targets will be built, but not installed. "
                   "You can specify the path manually by setting RTS_INSTALL_PREFIX_ZEROHOUR.")
endif()
