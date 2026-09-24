# Reuses rts_producer_test_extract from PerformanceReceiptProducerTitleTest.cmake.
# Attach to an existing x64 native fixture target, never the VC6/game target.
function(rts_add_native_receipt_trace_file_test target)
    if(NOT TARGET ${target} OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT WIN32)
        message(FATAL_ERROR "Held-file fixture requires an existing Windows x64 target")
    endif()
    if(NOT COMMAND rts_producer_test_extract)
        message(FATAL_ERROR "Load the existing exact source-extraction helper first")
    endif()
    set(_owner "${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/Common/NativeReceiptTraceFiles.inc")
    set(_runtime "${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/Common/ReplaySimulation.cpp")
    set(_receipt_fixture "${CMAKE_SOURCE_DIR}/Core/Tools/PerformanceReceiptTest/PerformanceReceiptTest.cpp")
    set(_test "${CMAKE_SOURCE_DIR}/Core/Tools/Stage5PerformanceFixtureTest/NativeReceiptTraceFileTest.cpp")
    foreach(_required IN ITEMS "${_owner}" "${_runtime}" "${_receipt_fixture}" "${_test}")
        if(NOT EXISTS "${_required}")
            message(FATAL_ERROR "Actual owner/fixture not promoted: ${_required}; do not substitute a test owner")
        endif()
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_owner}" "${_runtime}" "${_receipt_fixture}")
    file(READ "${_runtime}" _runtime_text)
    string(REGEX MATCHALL "#[ \t]*include[ \t]+\"NativeReceiptTraceFiles[.]inc\"" _runtime_includes "${_runtime_text}")
    list(LENGTH _runtime_includes _runtime_include_count)
    if(NOT _runtime_include_count EQUAL 1)
        message(FATAL_ERROR "The actual Runtime must include the single private file owner exactly once")
    endif()
    file(READ "${_owner}" _owner_text)
    rts_producer_test_extract(_owner_body "${_owner_text}"
        "// BEGIN RTS_NATIVE_RECEIPT_TRACE_FILES_PRIVATE"
        "// END RTS_NATIVE_RECEIPT_TRACE_FILES_PRIVATE")
    file(READ "${_receipt_fixture}" _receipt_text)
    # The entire existing fixture dependency closure is copied, not translated.
    # No line-number slicing, fake OS identity, cloned trace bytes or wire parser.
    rts_producer_test_extract(_complete_receipt "${_receipt_text}"
        "rts::performance::PerformanceReceipt makeCompleteReceipt()"
        "bool writeAtomicFixtureFile(")
    rts_producer_test_extract(_digest_text "${_receipt_text}"
        "KernelPerformanceDigest phaseWireDigest(unsigned char first)"
        "KernelPerformanceDigest phaseWireRunIdentity(")
    rts_producer_test_extract(_clock "${_receipt_text}"
        "struct ReceiptWindowClock"
        "int testActualControlEngineReceiptProjection()")
    rts_producer_test_extract(_actual_source "${_receipt_text}"
        "bool makeActualWorldTraceSourceReceipt("
        "int testActualWorldTraceReceiptProjection()")
    set(_generated "${CMAKE_CURRENT_BINARY_DIR}/${target}-held-file-source")
    file(MAKE_DIRECTORY "${_generated}")
    file(WRITE "${_generated}/NativeReceiptTraceFilesUnderTest.inc" "${_owner_body}")
    file(WRITE "${_generated}/NativeReceiptActualSourceFixture.inc"
        "${_complete_receipt}\n${_digest_text}\n${_clock}\n${_actual_source}")
    file(SHA256 "${_owner}" _owner_sha)
    file(SHA256 "${_receipt_fixture}" _fixture_sha)
    message(STATUS "Held-file actual owner SHA256=${_owner_sha}; real receipt/A fixture SHA256=${_fixture_sha}")
    target_sources(${target} PRIVATE "${_test}")
    target_include_directories(${target} PRIVATE "${_generated}" "${CMAKE_SOURCE_DIR}/Core/Tools/TestSupport")
    target_link_libraries(${target} PRIVATE core_task_runtime bcrypt)
    target_compile_features(${target} PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(${target} PRIVATE /EHsc)
    endif()

    if("${target}" STREQUAL "g_skirmish_ai_runner_contract_tests")
        set(_title_prefix g)
    elseif("${target}" STREQUAL "z_runtime_regression_tests")
        set(_title_prefix z)
    else()
        message(FATAL_ERROR "Held-file wrapper permits only the two existing native regression utilities")
    endif()
	set(_scratch_root "${CMAKE_BINARY_DIR}/s5-native-receipt-tests")
	file(MAKE_DIRECTORY "${_scratch_root}")
    foreach(_group IN ITEMS basic aliases)
        set(_test_name "${_title_prefix}_performance_receipt_files_${_group}_tests")
        add_test(NAME ${_test_name}
            COMMAND powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/Core/Tools/Stage5PerformanceFixtureTest/Invoke-NativeReceiptTraceFileTest.ps1"
                -UtilityPath "$<TARGET_FILE:${target}>"
                -Group "${_group}")
        set_tests_properties(${_test_name} PROPERTIES
            RUN_SERIAL TRUE PROCESSORS 1 TIMEOUT 30
			ENVIRONMENT "RTS_STAGE5_VALIDATION_SCRATCH_ROOT=${_scratch_root}")
        if(_group STREQUAL "aliases")
            # A host without symlink privilege reports the fixture's explicit
            # prerequisite code. Keep the capability gate visible as skipped;
            # never enable Developer Mode or elevate from the test harness.
            set_tests_properties(${_test_name} PROPERTIES SKIP_RETURN_CODE 2)
        endif()
    endforeach()
endfunction()
