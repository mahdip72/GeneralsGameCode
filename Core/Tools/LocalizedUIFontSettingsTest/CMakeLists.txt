add_executable(core_localized_ui_font_settings_tests
    LocalizedUIFontSettingsTest.cpp
)

target_include_directories(core_localized_ui_font_settings_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/GameEngine/Include
)

set_target_properties(core_localized_ui_font_settings_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_localized_ui_font_settings_tests
    COMMAND core_localized_ui_font_settings_tests)

if(WIN32 AND NOT IS_VS6_BUILD)
    find_program(LOCALIZED_UI_FONT_POWERSHELL NAMES pwsh powershell)
    if(LOCALIZED_UI_FONT_POWERSHELL)
        set(_localized_ui_font_generals_fragment
            "${CMAKE_CURRENT_BINARY_DIR}/LocalizedUIFontRefresh_Generals.inl")
        set(_localized_ui_font_generalsmd_fragment
            "${CMAKE_CURRENT_BINARY_DIR}/LocalizedUIFontRefresh_GeneralsMD.inl")

        add_custom_command(
            OUTPUT
                "${_localized_ui_font_generals_fragment}"
                "${_localized_ui_font_generalsmd_fragment}"
            COMMAND "${LOCALIZED_UI_FONT_POWERSHELL}"
                -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/ExtractLocalizedUIFontRefreshMethods.ps1"
                -SourceRoot "${CMAKE_SOURCE_DIR}"
                -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}"
            DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/ExtractLocalizedUIFontRefreshMethods.ps1"
                "${CMAKE_CURRENT_SOURCE_DIR}/LocalizedUIFontRefreshMockTypes.inl"
                "${CMAKE_CURRENT_SOURCE_DIR}/LocalizedUIFontRefreshAssertions.inl"
                "${CMAKE_SOURCE_DIR}/Generals/Code/GameEngine/Source/GameClient/InGameUI.cpp"
                "${CMAKE_SOURCE_DIR}/Generals/Code/GameEngine/Source/GameClient/Drawable.cpp"
                "${CMAKE_SOURCE_DIR}/GeneralsMD/Code/GameEngine/Source/GameClient/InGameUI.cpp"
                "${CMAKE_SOURCE_DIR}/GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp"
            VERBATIM
        )

        add_custom_target(core_localized_ui_font_refresh_fragments
            DEPENDS
                "${_localized_ui_font_generals_fragment}"
                "${_localized_ui_font_generalsmd_fragment}"
        )

        add_executable(core_localized_ui_font_refresh_tests
            LocalizedUIFontRefreshTest.cpp
        )

        add_dependencies(core_localized_ui_font_refresh_tests
            core_localized_ui_font_refresh_fragments)

        target_include_directories(core_localized_ui_font_refresh_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}"
            "${CMAKE_SOURCE_DIR}/Core/GameEngine/Include"
        )

        target_compile_features(core_localized_ui_font_refresh_tests PRIVATE cxx_std_20)

        add_test(NAME core_localized_ui_font_refresh_tests
            COMMAND core_localized_ui_font_refresh_tests)

        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(_resolution_refresh_generals_fragment
                "${CMAKE_CURRENT_BINARY_DIR}/ResolutionRefreshCallbacks_Generals.inl")
            set(_resolution_refresh_generalsmd_fragment
                "${CMAKE_CURRENT_BINARY_DIR}/ResolutionRefreshCallbacks_GeneralsMD.inl")

            add_custom_target(core_resolution_refresh_callback_fragments
                COMMAND "${LOCALIZED_UI_FONT_POWERSHELL}"
                    -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/ExtractResolutionRefreshCallbacks.ps1"
                    -SourceRoot "${CMAKE_SOURCE_DIR}"
                    -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}"
                BYPRODUCTS
                    "${_resolution_refresh_generals_fragment}"
                    "${_resolution_refresh_generalsmd_fragment}"
                DEPENDS
                    "${CMAKE_CURRENT_SOURCE_DIR}/ExtractResolutionRefreshCallbacks.ps1"
                    "${CMAKE_SOURCE_DIR}/Generals/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/MainMenu.cpp"
                    "${CMAKE_SOURCE_DIR}/Generals/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/OptionsMenu.cpp"
                    "${CMAKE_SOURCE_DIR}/GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/MainMenu.cpp"
                    "${CMAKE_SOURCE_DIR}/GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/OptionsMenu.cpp"
                VERBATIM
            )

            add_executable(core_resolution_refresh_callback_tests
                ResolutionRefreshCallbackTest.cpp)
            add_dependencies(core_resolution_refresh_callback_tests
                core_resolution_refresh_callback_fragments)
            target_include_directories(core_resolution_refresh_callback_tests PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}"
                "${CMAKE_CURRENT_BINARY_DIR}")
            target_compile_features(core_resolution_refresh_callback_tests PRIVATE cxx_std_20)
            add_test(NAME core_resolution_refresh_callback_tests
                COMMAND core_resolution_refresh_callback_tests)
        endif()
    endif()
endif()

option(CORE_LOCALIZED_UI_FONT_REFRESH_ENABLE_STALE_BASELINE_QUALIFICATION
    "Build the optional historical stale-font baseline qualification (requires a local Git object)"
    OFF)

if(WIN32 AND NOT IS_VS6_BUILD AND CORE_LOCALIZED_UI_FONT_REFRESH_ENABLE_STALE_BASELINE_QUALIFICATION)
    set(CORE_LOCALIZED_UI_FONT_HISTORICAL_BASELINE_REVISION "" CACHE STRING
        "Git revision containing the pre-refresh localized font initialization")
    if(NOT CORE_LOCALIZED_UI_FONT_HISTORICAL_BASELINE_REVISION)
        message(FATAL_ERROR
            "Set CORE_LOCALIZED_UI_FONT_HISTORICAL_BASELINE_REVISION to opt into historical baseline qualification.")
    endif()
    find_program(LOCALIZED_UI_FONT_STALE_BASELINE_POWERSHELL NAMES pwsh powershell)
    if(NOT LOCALIZED_UI_FONT_STALE_BASELINE_POWERSHELL)
        message(FATAL_ERROR "Historical baseline qualification requires PowerShell.")
    endif()

    set(_localized_ui_font_stale_generals_fragment
        "${CMAKE_CURRENT_BINARY_DIR}/LocalizedUIFontStaleBaseline_Generals.inl")
    set(_localized_ui_font_stale_generalsmd_fragment
        "${CMAKE_CURRENT_BINARY_DIR}/LocalizedUIFontStaleBaseline_GeneralsMD.inl")

    add_custom_command(
        OUTPUT
            "${_localized_ui_font_stale_generals_fragment}"
            "${_localized_ui_font_stale_generalsmd_fragment}"
        COMMAND "${LOCALIZED_UI_FONT_STALE_BASELINE_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/ExtractLocalizedUIFontStaleBaseline.ps1"
            -SourceRoot "${CMAKE_SOURCE_DIR}"
            -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}"
            -BaselineRevision "${CORE_LOCALIZED_UI_FONT_HISTORICAL_BASELINE_REVISION}"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/ExtractLocalizedUIFontStaleBaseline.ps1"
            "${CMAKE_CURRENT_SOURCE_DIR}/LocalizedUIFontRefreshMockTypes.inl"
        VERBATIM
    )

    add_custom_target(core_localized_ui_font_stale_baseline_fragments
        DEPENDS
            "${_localized_ui_font_stale_generals_fragment}"
            "${_localized_ui_font_stale_generalsmd_fragment}")

    add_executable(core_localized_ui_font_stale_baseline_qualification
        LocalizedUIFontStaleBaselineQualification.cpp)
    add_dependencies(core_localized_ui_font_stale_baseline_qualification
        core_localized_ui_font_stale_baseline_fragments)
    target_include_directories(core_localized_ui_font_stale_baseline_qualification PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}"
        "${CMAKE_SOURCE_DIR}/Core/GameEngine/Include")
    target_compile_features(core_localized_ui_font_stale_baseline_qualification PRIVATE cxx_std_20)
    add_test(NAME core_localized_ui_font_stale_baseline_qualification
        COMMAND core_localized_ui_font_stale_baseline_qualification)
endif()
