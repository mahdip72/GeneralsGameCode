add_executable(core_shroud_destination_update_tests
    ShroudDestinationUpdateTest.cpp
    ../../GameEngineDevice/Source/W3DDevice/Common/System/W3DShroudRenderPolicy.cpp
)

# Avoid legacy Windows installer detection for manifest-less VC6 binaries named "*update*".
set_target_properties(core_shroud_destination_update_tests PROPERTIES
    OUTPUT_NAME core_shroud_destination_policy_tests
)

target_include_directories(core_shroud_destination_update_tests PRIVATE
    ../../GameEngineDevice/Include
)

add_test(NAME core_shroud_destination_update_tests
    COMMAND core_shroud_destination_update_tests
)
