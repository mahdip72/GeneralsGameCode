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
