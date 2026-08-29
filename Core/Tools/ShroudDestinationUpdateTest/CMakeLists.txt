add_executable(core_shroud_destination_update_tests
    ShroudDestinationUpdateTest.cpp
    ../../GameEngineDevice/Source/W3DDevice/Common/System/W3DShroudRenderPolicy.cpp
)

target_include_directories(core_shroud_destination_update_tests PRIVATE
    ../../GameEngineDevice/Include
)

if(IS_VS6_BUILD)
    # Avoid the legacy Windows installer-name heuristic, which requests
    # elevation for unmanifested VC6 executables containing "update".
    set_target_properties(core_shroud_destination_update_tests PROPERTIES
        OUTPUT_NAME core_shroud_destination_policy_tests
    )
endif()

add_test(NAME core_shroud_destination_update_tests
    COMMAND core_shroud_destination_update_tests
)
