add_executable(core_shroud_destination_update_tests
    ShroudDestinationUpdateTest.cpp
    ../../GameEngineDevice/Source/W3DDevice/Common/System/W3DShroudRenderPolicy.cpp
)

target_include_directories(core_shroud_destination_update_tests PRIVATE
    ../../GameEngineDevice/Include
)

add_test(NAME core_shroud_destination_update_tests
    COMMAND core_shroud_destination_update_tests
)
