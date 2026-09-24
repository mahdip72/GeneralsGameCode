if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND WIN32 AND NOT IS_VS6_BUILD AND
	TARGET core_ww3d2_native)
	add_executable(core_native_w3d_line_tests
		NativeW3DLineTest.cpp
	)

	target_include_directories(core_native_w3d_line_tests PRIVATE
		"${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WW3D2"
	)

	target_link_libraries(core_native_w3d_line_tests PRIVATE
		core_ww3d2_native
		core_wwstub
		core_wwlib
		winmm
	)

	set_target_properties(core_native_w3d_line_tests PROPERTIES
		CXX_STANDARD 20
		CXX_STANDARD_REQUIRED YES
		CXX_EXTENSIONS NO
	)

	add_test(NAME core_native_w3d_line_tests
		COMMAND core_native_w3d_line_tests
	)
	set_tests_properties(core_native_w3d_line_tests PROPERTIES TIMEOUT 30)
endif()
