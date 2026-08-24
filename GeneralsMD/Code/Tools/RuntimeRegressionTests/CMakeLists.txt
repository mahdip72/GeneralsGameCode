if(CMAKE_SIZEOF_VOID_P EQUAL 4)
add_executable(z_runtime_regression_tests)

target_sources(z_runtime_regression_tests PRIVATE
    RuntimeRegressionTests.cpp
)

target_link_libraries(z_runtime_regression_tests PRIVATE
	comctl32
	core_debug
	core_profile_legacy
	d3d8
	d3dx8
	dinput8
	dxguid
	imm32
	milesstub
	vfw32
	winmm
	z_gameengine
	z_gameenginedevice
	zi_always
)

if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND NOT RTS_BUILD_OPTION_FFMPEG)
	target_link_libraries(z_runtime_regression_tests PRIVATE binkstub)
endif()

add_test(NAME z_skirmish_ai_replay_epoch_tests COMMAND z_runtime_regression_tests --skirmish-ai-replay-epoch)
set_tests_properties(z_skirmish_ai_replay_epoch_tests PROPERTIES TIMEOUT 30)

add_test(NAME z_skirmish_ai_runner_contract_tests COMMAND z_runtime_regression_tests --skirmish-ai-runner-contract)
set_tests_properties(z_skirmish_ai_runner_contract_tests PROPERTIES TIMEOUT 30)
endif()
