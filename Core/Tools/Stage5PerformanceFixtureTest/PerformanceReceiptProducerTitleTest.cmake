include_guard(GLOBAL)

function(rts_producer_test_extract output text start_marker end_marker)
    string(FIND "${text}" "${start_marker}" _start)
    string(FIND "${text}" "${end_marker}" _first_end)
    if(_start LESS 0 OR _first_end LESS _start)
        message(FATAL_ERROR "Producer fixture source boundary missing or reversed: ${start_marker}")
    endif()
    string(SUBSTRING "${text}" ${_start} -1 _tail)
    string(LENGTH "${start_marker}" _start_length)
    string(SUBSTRING "${_tail}" ${_start_length} -1 _after_start)
    string(FIND "${_after_start}" "${start_marker}" _duplicate_start)
    string(FIND "${_tail}" "${end_marker}" _end)
    string(SUBSTRING "${_tail}" ${_end} -1 _end_tail)
    string(LENGTH "${end_marker}" _end_length)
    string(SUBSTRING "${_end_tail}" ${_end_length} -1 _after_end)
    string(FIND "${_after_end}" "${end_marker}" _duplicate_end)
    if(NOT _duplicate_start EQUAL -1 OR NOT _duplicate_end EQUAL -1)
        message(FATAL_ERROR "Producer fixture source boundary ambiguous: ${start_marker}")
    endif()
    string(SUBSTRING "${_tail}" 0 ${_end} _body)
    set(${output} "${_body}" PARENT_SCOPE)
endfunction()

function(rts_add_performance_receipt_producer_test target title test_name)
    if(NOT title STREQUAL "Generals" AND NOT title STREQUAL "GeneralsMD")
        message(FATAL_ERROR "Producer fixture requires one exact native title")
    endif()
    set(_replay "${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/Common/ReplaySimulation.cpp")
	set(_native_trace "${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/Common/NativeReceiptTraceFiles.inc")
    set(_owner "${CMAKE_SOURCE_DIR}/${title}/Code/GameEngine/Source/GameLogic/System/GameLogic.cpp")
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
		"${_replay}" "${_native_trace}" "${_owner}")
    file(READ "${_replay}" _replay_text)
	file(READ "${_native_trace}" _native_trace_text)
    file(READ "${_owner}" _owner_text)
    string(REPLACE "\r\n" "\n" _replay_text "${_replay_text}")
	string(REPLACE "\r\n" "\n" _native_trace_text "${_native_trace_text}")
    string(REPLACE "\r\n" "\n" _owner_text "${_owner_text}")

    rts_producer_test_extract(_owner_methods "${_owner_text}"
        "rts::LiveSimulationPhaseOwnerCallbacks GameLogic::makeStage5PhaseGraphCallbacks()"
        "Bool GameLogic::runOwnerIntakePhase(")
    rts_producer_test_extract(_metric_copy "${_replay_text}"
        "void appendPerformanceReceiptPhase("
        "bool resolvePerformanceReceiptTimingPath(")
	rts_producer_test_extract(_raw_producer "${_replay_text}"
		"static const char *performanceReceiptRawProducer("
		"#endif\n\nvoid printHeadlessJobMetrics(")
	rts_producer_test_extract(_finish "${_replay_text}"
		"void PerformanceReceiptRuntime::finish("
		"int ReplaySimulation::simulateReplaysInThisProcess(")
	string(FIND "${_finish}" "m_receipt.kernelTiming =" _timing_freeze)
	string(FIND "${_finish}" "m_receipt.kernelReference =" _reference_freeze)
	string(FIND "${_finish}" "projectPerformanceReceiptBaselineMetrics(m_receipt);" _projection)
	string(FIND "${_finish}" "resolvePerformanceReceiptTimingPath(m_receipt" _timing_resolution)
	if(_timing_freeze LESS 0 OR _reference_freeze LESS 0 OR _projection LESS 0 OR
		_timing_resolution LESS 0 OR _timing_freeze GREATER _reference_freeze OR
		_reference_freeze GREATER _projection OR _projection GREATER _timing_resolution)
		message(FATAL_ERROR
			"Receipt finish must project baseline metrics after both freezes and before publication timing closure")
	endif()
    rts_producer_test_extract(_capture "${_replay_text}"
        "void PerformanceReceiptRuntime::captureCompletedFrame("
        "void PerformanceReceiptRuntime::captureTerminalResult(")
    # Rename only the entry point. Its signature suffix and entire body remain
    # source-exact; the fixture view adds ordering checks around this call.
    string(REPLACE "void PerformanceReceiptRuntime::captureCompletedFrame("
        "void PerformanceReceiptRuntime::captureCompletedFrameFromSource("
        _capture "${_capture}")
    rts_producer_test_extract(_source_hold "${_replay_text}"
        "class ImmutableReplayReceiptSource"
        "void printHeadlessReplaySliceMetrics(")
	rts_producer_test_extract(_native_path_identity "${_native_trace_text}"
		"namespace native_receipt_files\n{" "class Sha256")
	string(APPEND _native_path_identity "\n} // namespace native_receipt_files\n")
    rts_producer_test_extract(_job_scope "${_replay_text}"
        "class HeadlessSimulationJobSystemScope"
        "int countProcessesRunning(")
    rts_producer_test_extract(_loop "${_replay_text}"
        "int ReplaySimulation::simulateReplaysInThisProcess("
        "int ReplaySimulation::simulateReplaysInWorkerProcesses(")
    rts_producer_test_extract(_mode_names "${_replay_text}"
        "const char *simulationModeName("
        "#if defined(_WIN64)\n// Prevent replacement or mutation")
    rts_producer_test_extract(_slice_print "${_replay_text}"
        "void printHeadlessReplaySliceMetrics("
        "void appendPerformanceReceiptPhase(")
    rts_producer_test_extract(_job_print "${_replay_text}"
        "void printHeadlessJobMetrics("
        "class HeadlessSimulationJobSystemScope")
    rts_producer_test_extract(_count_processes "${_replay_text}"
        "int countProcessesRunning(" "} // namespace")
    rts_producer_test_extract(_worker_loop "${_replay_text}"
        "int ReplaySimulation::simulateReplaysInWorkerProcesses("
        "std::vector<AsciiString> ReplaySimulation::resolveFilenameWildcards(")
    rts_producer_test_extract(_resolve_wildcards "${_replay_text}"
        "std::vector<AsciiString> ReplaySimulation::resolveFilenameWildcards("
        "int ReplaySimulation::simulateReplays(")
    rts_producer_test_extract(_public_route "${_replay_text}\nRTS_REPLAY_OWNER_SHAPE_FIXTURE_EOF"
        "int ReplaySimulation::simulateReplays(" "RTS_REPLAY_OWNER_SHAPE_FIXTURE_EOF")

    # The replay source identity must stay bound to the one held native
    # handle. A pathname hash would reopen a replaceable/aliased file and
    # silently decouple the receipt digest from the replay that is consumed.
    string(FIND "${_source_hold}" "HashSkirmishAITestContentFile(" _source_path_hash)
    string(FIND "${_source_hold}" "HashSkirmishAITestContentHandle(" _source_handle_hash)
	string(FIND "${_source_hold}" "openDirectoryComponents(" _source_ancestor_hold)
	string(FIND "${_source_hold}" "bool finish()" _source_checked_closure)
	string(FIND "${_loop}" "performanceSource.finish()" _loop_source_closure)
	string(FIND "${_loop}" "performanceReceipt.finish(" _loop_receipt_finish)
	if(NOT _source_path_hash EQUAL -1 OR _source_handle_hash LESS 0 OR
		_source_ancestor_hold LESS 0 OR _source_checked_closure LESS 0 OR
		_loop_source_closure LESS 0 OR _loop_receipt_finish LESS 0 OR
		_loop_source_closure GREATER _loop_receipt_finish)
		message(FATAL_ERROR
			"Immutable replay source must hold canonical ancestors and close its identity-bound handle before receipt publication")
    endif()
    # Explicit native ownership must be rejected before the public wildcard
    # expansion can enumerate a directory or accidentally resolve to one file.
    string(FIND "${_public_route}" "explicitTraceRequestedFromEnvironment()" _public_intent)
    string(FIND "${_public_route}" "resolveFilenameWildcards(filenames)" _public_expansion)
    string(FIND "${_public_route}" "concreteSingleReplay" _public_concrete_shape)
    if(_public_intent LESS 0 OR _public_expansion LESS 0 OR _public_concrete_shape LESS 0 OR
        _public_intent GREATER _public_expansion)
        message(FATAL_ERROR "Replay owner shape guard must preflight concrete shape before wildcard expansion")
    endif()

    set(_out "${CMAKE_CURRENT_BINARY_DIR}/performance-receipt-producer")
    file(MAKE_DIRECTORY "${_out}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptProducerOwnerMethods.inc" CONTENT "${_owner_methods}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptProducerMetricCopy.inc" CONTENT "${_metric_copy}")
	file(GENERATE OUTPUT "${_out}/PerformanceReceiptRawProducer.inc" CONTENT "${_raw_producer}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptProducerCapture.inc" CONTENT "${_capture}")
	file(GENERATE OUTPUT "${_out}/PerformanceReceiptNativePathIdentity.inc"
		CONTENT "${_native_path_identity}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptProducerSourceHold.inc" CONTENT "${_source_hold}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptProducerJobScope.inc" CONTENT "${_job_scope}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptProducerLoop.inc" CONTENT "${_loop}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptProducerCountProcesses.inc" CONTENT "${_count_processes}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptProducerWorkerLoop.inc" CONTENT "${_worker_loop}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptProducerResolveWildcards.inc" CONTENT "${_resolve_wildcards}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptProducerPublicRoute.inc" CONTENT "${_public_route}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptProducerPrintMetrics.inc"
        CONTENT "${_mode_names}\n${_slice_print}\n${_job_print}")
    target_sources(${target} PRIVATE
        "${CMAKE_SOURCE_DIR}/Core/Tools/Stage5PerformanceFixtureTest/PerformanceReceiptProducerTitleTest.cpp")
    target_include_directories(${target} PRIVATE "${_out}")
    add_test(NAME ${test_name} COMMAND ${target} --performance-receipt-producer)
    set_tests_properties(${test_name} PROPERTIES
        TIMEOUT 30 WORKING_DIRECTORY "${_out}")
endfunction()

function(rts_add_performance_receipt_fresh_producer_test target title test_name)
    if(NOT title STREQUAL "Generals" AND NOT title STREQUAL "GeneralsMD")
        message(FATAL_ERROR "Fresh producer fixture requires one exact native title")
    endif()
    set(_owner "${CMAKE_SOURCE_DIR}/${title}/Code/GameEngine/Source/GameLogic/System/GameLogic.cpp")
    set(_engine "${CMAKE_SOURCE_DIR}/${title}/Code/GameEngine/Source/Common/GameEngine.cpp")
    set(_main "${CMAKE_SOURCE_DIR}/${title}/Code/GameEngine/Source/Common/GameMain.cpp")
    set(_runner "${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/Common/SkirmishAITestRunner.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_owner}" "${_engine}" "${_main}" "${_runner}")
    foreach(_source IN ITEMS owner engine main runner)
        file(READ "${_${_source}}" _${_source}_text)
        string(REPLACE "\r\n" "\n" _${_source}_text "${_${_source}_text}")
    endforeach()

    rts_producer_test_extract(_owner_methods "${_owner_text}"
        "bool GameLogic::isStage5PhaseGraphOwner("
        "bool GameLogic::validateStage5PhaseGraphCommit(")
    rts_producer_test_extract(_runner_state "${_runner_text}"
        "struct SkirmishAITestRunnerState"
        "void CaptureSkirmishAITestSliceMetrics();")
    rts_producer_test_extract(_slice_capture "${_runner_text}"
        "void CaptureSkirmishAITestSliceMetrics()\n{"
        "void PrintJobMetric(")
    rts_producer_test_extract(_runtime_capture "${_runner_text}"
        "void CaptureSkirmishAITestRuntimeState()"
        "#if defined(_WIN64)\nstruct ClosePerformanceMapFile")
    rts_producer_test_extract(_start "${_runner_text}"
        "Bool StartSkirmishAITestRunner()"
        "void UpdateSkirmishAITestRunner()")
    rts_producer_test_extract(_fail "${_runner_text}"
        "void FailSkirmishAITest(const char *reason)"
        "void RequestSkirmishAITestStop()")
    # The finalizer block is deliberately source-exact through EOF. The
    # sentinel exists only in memory and remains subject to unique-boundary checks.
    rts_producer_test_extract(_finalizers "${_runner_text}\nRTS_FRESH_PRODUCER_FIXTURE_EOF"
        "#if defined(_WIN64)\nvoid ObserveSkirmishAITestCompletedFrame("
        "RTS_FRESH_PRODUCER_FIXTURE_EOF")
    rts_producer_test_extract(_engine_destructor "${_engine_text}"
        "GameEngine::~GameEngine()"
        "Bool GameEngine::isTimeFrozen()")
    rts_producer_test_extract(_main_cleanup "${_main_text}"
        "\tif (IsSkirmishAITestRunnerArmed())"
        "\treturn exitcode;")
    rts_producer_test_extract(_main_start "${_main_text}"
        "\tconst Bool canRun = !validationOptionsConflict && !net3ValidationRequested &&"
        "\tif (IsSkirmishAITestRunnerArmed())")

    set(_out "${CMAKE_CURRENT_BINARY_DIR}/performance-receipt-fresh-producer")
    file(MAKE_DIRECTORY "${_out}")
    file(GENERATE OUTPUT "${_out}/FreshProducerOwnerMethods.inc" CONTENT "${_owner_methods}")
    file(GENERATE OUTPUT "${_out}/FreshProducerRunnerState.inc" CONTENT "${_runner_state}")
    file(GENERATE OUTPUT "${_out}/FreshProducerCaptureState.inc"
        CONTENT "${_slice_capture}\n${_runtime_capture}")
    file(GENERATE OUTPUT "${_out}/FreshProducerStart.inc" CONTENT "${_start}")
    file(GENERATE OUTPUT "${_out}/FreshProducerFailure.inc" CONTENT "${_fail}")
    file(GENERATE OUTPUT "${_out}/FreshProducerGameMainStart.inc" CONTENT "${_main_start}")
    file(GENERATE OUTPUT "${_out}/FreshProducerFinalizers.inc" CONTENT "${_finalizers}")
    file(GENERATE OUTPUT "${_out}/FreshProducerEngineDestructor.inc" CONTENT "${_engine_destructor}")
    file(GENERATE OUTPUT "${_out}/FreshProducerGameMainCleanup.inc" CONTENT "${_main_cleanup}")
    target_sources(${target} PRIVATE
        "${CMAKE_SOURCE_DIR}/Core/Tools/Stage5PerformanceFixtureTest/PerformanceReceiptFreshProducerTitleTest.cpp")
    target_include_directories(${target} PRIVATE "${_out}")
    add_test(NAME ${test_name} COMMAND ${target} --performance-receipt-fresh-producer)
    set_tests_properties(${test_name} PROPERTIES
        TIMEOUT 30 WORKING_DIRECTORY "${_out}")
endfunction()

function(rts_add_performance_receipt_attempt_test target title test_name)
    if(NOT title STREQUAL "Generals" AND NOT title STREQUAL "GeneralsMD")
        message(FATAL_ERROR "Attempt fixture requires one exact native title")
    endif()
    set(_owner "${CMAKE_SOURCE_DIR}/${title}/Code/GameEngine/Source/GameLogic/System/GameLogic.cpp")
    set(_runtime "${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/Common/ReplaySimulation.cpp")
	set(_spatial "${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/GameLogic/System/ImmutableSpatialQueryRuntime.cpp")
	set(_path_owner "${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/GameLogic/AI/AIPathfind.cpp")
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
		"${_owner}" "${_runtime}" "${_spatial}" "${_path_owner}")
    file(READ "${_owner}" _owner_text)
    file(READ "${_runtime}" _runtime_text)
	file(READ "${_spatial}" _spatial_text)
	file(READ "${_path_owner}" _path_owner_text)
    string(REPLACE "\r\n" "\n" _owner_text "${_owner_text}")
    string(REPLACE "\r\n" "\n" _runtime_text "${_runtime_text}")
	string(REPLACE "\r\n" "\n" _spatial_text "${_spatial_text}")
	string(REPLACE "\r\n" "\n" _path_owner_text "${_path_owner_text}")
    rts_producer_test_extract(_forwarder "${_owner_text}"
        "rts::performance::KernelPerformanceAttempt GameLogic::beginPerformanceReceiptAttempt("
        "\n#endif\n\nrts::LiveSimulationPhaseOwnerCallbacks GameLogic::makeStage5PhaseGraphCallbacks()")
    rts_producer_test_extract(_attempt "${_runtime_text}"
        "rts::performance::KernelPerformanceAttempt PerformanceReceiptRuntime::beginAttempt("
        "static rts::performance::KernelPerformanceSchedulerBoundary\ncapturePerformancePhaseSchedulerBoundary()")
	rts_producer_test_extract(_status_guard "${_owner_text}"
		"struct DeferredKernelPerformanceFallback"
		"struct KernelPerformanceIntervalGuard")
	rts_producer_test_extract(_status_tail "${_owner_text}"
		"void GameLogic::runOwnerTailPhase()"
		"void GameLogic::runVerificationAndPublicationPhase()")
	string(FIND "${_status_guard}" "deferredFallback->capture(" _status_defer)
	string(FIND "${_status_guard}" "false, false);" _status_discard)
	string(FIND "${_status_tail}" "runPreparedDisabledStatusSweep(this, &statusFallback)" _status_prepare)
	string(FIND "${_status_tail}" "runLegacyDisabledStatusSweep(this);" _status_legacy)
	string(FIND "${_status_tail}" "statusFallback.finish();" _status_finish)
	if(_status_defer LESS 0 OR _status_discard LESS 0 OR _status_prepare LESS 0 OR
		_status_legacy LESS _status_prepare OR _status_finish LESS _status_legacy)
		message(FATAL_ERROR
			"Status receipt fallback must remain open through the actual legacy sweep while validated discard stays non-fallback")
	endif()
	rts_producer_test_extract(_spatial_post_dispatch "${_spatial_text}"
		"const rts::ImmutableSpatialJobSystemResult result ="
		"LiveImmutableSpatialQueryResult query(")
	rts_producer_test_extract(_spatial_completion "${_spatial_text}"
		"void completeConsumer(LiveImmutableSpatialConsumer consumer,"
		"void recordAuthoritative(LiveImmutableSpatialConsumer consumer,")
	string(FIND "${_spatial_post_dispatch}"
		"finishPerformanceBatch(fallbackPerformanceDisposition())" _spatial_early_finish)
	string(FIND "${_spatial_completion}" "m_performanceCompletion.allConsumersCommitted"
		_spatial_committed_gate)
	string(FIND "${_spatial_completion}" "KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION"
		_spatial_early_abort)
	string(FIND "${_spatial_text}"
		"void endCollection()\n\t{\n\t\tif (m_performanceBatchActive || m_referenceBatch.valid() ||\n\t\t\tm_referenceAttempt.valid())\n\t\t\tfinishPerformanceBatch(fallbackPerformanceDisposition());"
		_spatial_end_finish)
	if(NOT _spatial_early_finish EQUAL -1 OR _spatial_committed_gate LESS 0 OR
		NOT _spatial_early_abort EQUAL -1 OR _spatial_end_finish LESS 0)
		message(FATAL_ERROR
			"Spatial post-dispatch failures must retain receipt ownership until the collection end boundary")
	endif()
	string(REGEX MATCHALL
		"const Bool committed = performanceCompletion\\.committed\\(\\) \\? TRUE : FALSE"
		_path_full_commit_gates "${_path_owner_text}")
	list(LENGTH _path_full_commit_gates _path_full_commit_gate_count)
	string(REGEX MATCHALL
		"&entry->performanceReferenceOperationCommitted,\n\t\t&materializationBegan\\)"
		_path_materialization_guards "${_path_owner_text}")
	list(LENGTH _path_materialization_guards _path_materialization_guard_count)
	string(FIND "${_path_owner_text}" "performanceCommitted = TRUE;"
		_path_partial_commit)
	rts_producer_test_extract(_path_fallback_boundary "${_path_owner_text}"
		"const Bool directLegacyFallback ="
		"if (pat == nullptr)")
	string(FIND "${_path_fallback_boundary}"
		"BeginPathPerformanceLegacyFallback(m_directPathBatchContext)"
		_path_fallback_begin)
	string(FIND "${_path_fallback_boundary}"
		"Path *pat = internalFindPath(obj, locomotorSet, from, rawTo);"
		_path_legacy_call)
	string(FIND "${_path_fallback_boundary}"
		"CompletePathPerformanceLegacyFallback(m_directPathBatchContext,"
		_path_fallback_complete)
	if(NOT _path_full_commit_gate_count EQUAL 2 OR
		NOT _path_materialization_guard_count EQUAL 2 OR
		NOT _path_partial_commit EQUAL -1 OR _path_fallback_begin LESS 0 OR
		_path_legacy_call LESS _path_fallback_begin OR
		_path_fallback_complete LESS _path_legacy_call)
		message(FATAL_ERROR
			"Direct and ordinary path receipts must require full-batch authority and close fallback only around the actual legacy path call")
	endif()
    # Only the qualified entry name changes; the real method body runs on the
    # derived runtime with the fixture's world collaborator in scope.
    string(REPLACE "PerformanceReceiptRuntime::beginAttempt("
        "PerformanceReceiptRuntime::beginAttemptFromSource(" _attempt "${_attempt}")
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/performance-receipt-attempt")
    file(MAKE_DIRECTORY "${_out}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptAttemptOwnerForwarder.inc" CONTENT "${_forwarder}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptAttemptRuntime.inc" CONTENT "${_attempt}")
    target_sources(${target} PRIVATE
        "${CMAKE_SOURCE_DIR}/Core/Tools/Stage5PerformanceFixtureTest/PerformanceReceiptAttemptTitleTest.cpp")
    target_include_directories(${target} PRIVATE "${_out}")
    add_test(NAME ${test_name} COMMAND ${target} --performance-receipt-attempt)
    set_tests_properties(${test_name} PROPERTIES
        TIMEOUT 30 WORKING_DIRECTORY "${_out}")
endfunction()

function(rts_add_performance_receipt_bootstrap_test target title test_name)
    if(NOT title STREQUAL "Generals" AND NOT title STREQUAL "GeneralsMD")
        message(FATAL_ERROR "Bootstrap fixture requires one exact native title")
    endif()
    set(_owner "${CMAKE_SOURCE_DIR}/${title}/Code/GameEngine/Source/GameLogic/System/GameLogic.cpp")
    set(_header "${CMAKE_SOURCE_DIR}/${title}/Code/GameEngine/Include/GameLogic/GameLogic.h")
    set(_dispatch "${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/GameLogic/System/GameLogicDispatch.cpp")
    set(_runtime "${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/Common/ReplaySimulation.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_owner}" "${_header}" "${_dispatch}" "${_runtime}")
    foreach(_source IN ITEMS owner header dispatch runtime)
        file(READ "${_${_source}}" _${_source}_text)
        string(REPLACE "\r\n" "\n" _${_source}_text "${_${_source}_text}")
    endforeach()

    rts_producer_test_extract(_mode "${_header_text}"
        "enum GameMode CPP_11(: Int)" "const char* toString(GameMode mode);")
    rts_producer_test_extract(_new_game "${_dispatch_text}"
        "bool GameLogic::onNewGame(MAYBE_UNUSED GameMessage *msg)"
        "bool GameLogic::onClearGameData(")
    set(_defer_start "void GameLogic::tryStartNewGame( Bool loadingSaveGame )\n{")
    rts_producer_test_extract(_defer "${_owner_text}"
        "${_defer_start}" "\tm_rankLevelLimit = 1000;\t// this is reset every game.")
    # Strip only the outer function declaration/open brace. The full native
    # reset and first loadingSaveGame block, including its early return, remain.
    string(LENGTH "${_defer_start}" _defer_prefix_length)
    string(SUBSTRING "${_defer}" ${_defer_prefix_length} -1 _defer)
    rts_producer_test_extract(_intake "${_owner_text}"
        "Bool GameLogic::runOwnerIntakePhase(" "void GameLogic::runLegacyMutableIslandPhase(")
    rts_producer_test_extract(_resume "${_intake}"
        "\tif ( m_startNewGame && !TheDisplay->isMoviePlaying())"
        "\t// send the current time to the GameClient")
    rts_producer_test_extract(_verification "${_owner_text}"
        "void GameLogic::runVerificationAndPublicationPhase()" "void GameLogic::preUpdate()")
    # The column-zero closing brace is the outer function boundary; the
    # complete nested increment/lockstep branch is left source-exact.
    rts_producer_test_extract(_increment "${_verification}"
        "\tif (!m_startNewGame)" "\n}\n")
    rts_producer_test_extract(_scheduler "${_runtime_text}"
        "static rts::performance::KernelPerformanceSchedulerBoundary\ncapturePerformancePhaseSchedulerBoundary()"
        "static bool observePerformanceReferenceWindow(")

    set(_out "${CMAKE_CURRENT_BINARY_DIR}/performance-receipt-bootstrap")
    file(MAKE_DIRECTORY "${_out}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptBootstrapGameMode.inc" CONTENT "${_mode}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptBootstrapOnNewGame.inc" CONTENT "${_new_game}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptBootstrapDeferredPrefix.inc" CONTENT "${_defer}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptBootstrapResume.inc" CONTENT "${_resume}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptBootstrapIncrement.inc" CONTENT "${_increment}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptBootstrapSchedulerBoundary.inc" CONTENT "${_scheduler}")
    target_sources(${target} PRIVATE
        "${CMAKE_SOURCE_DIR}/Core/Tools/Stage5PerformanceFixtureTest/PerformanceReceiptBootstrapTitleTest.cpp")
    target_include_directories(${target} PRIVATE "${_out}")
    add_test(NAME ${test_name} COMMAND ${target} --performance-receipt-bootstrap)
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 30 WORKING_DIRECTORY "${_out}")
endfunction()

function(rts_add_performance_receipt_policy_test target test_name)
    set(_runtime "${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/Common/ReplaySimulation.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_runtime}")
    file(READ "${_runtime}" _runtime_text)
    string(REPLACE "\r\n" "\n" _runtime_text "${_runtime_text}")
    rts_producer_test_extract(_policy "${_runtime_text}"
        "static bool deriveNativeReceiptTraceCapacities("
        "PerformanceReceiptRuntime::PerformanceReceiptRuntime()")
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/performance-receipt-policy")
    file(MAKE_DIRECTORY "${_out}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptNativePolicy.inc" CONTENT "${_policy}")
    target_sources(${target} PRIVATE
        "${CMAKE_SOURCE_DIR}/Core/Tools/Stage5PerformanceFixtureTest/PerformanceReceiptPolicyTitleTest.cpp")
    target_include_directories(${target} PRIVATE "${_out}")
    add_test(NAME ${test_name} COMMAND ${target} --performance-receipt-policy)
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 30 WORKING_DIRECTORY "${_out}")
endfunction()

function(rts_add_performance_receipt_mode_test target test_name)
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/performance-receipt-mode")
    file(MAKE_DIRECTORY "${_out}")
    target_sources(${target} PRIVATE
        "${CMAKE_SOURCE_DIR}/Core/Tools/Stage5PerformanceFixtureTest/PerformanceReceiptModeTitleTest.cpp")
    add_test(NAME ${test_name} COMMAND ${target} --performance-receipt-mode)
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 30 WORKING_DIRECTORY "${_out}")
endfunction()

function(rts_add_performance_receipt_wide_path_test target test_name)
    set(_runtime "${CMAKE_SOURCE_DIR}/Core/GameEngine/Source/Common/ReplaySimulation.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_runtime}")
    file(READ "${_runtime}" _runtime_text)
    string(REPLACE "\r\n" "\n" _runtime_text "${_runtime_text}")
    rts_producer_test_extract(_wide_inputs "${_runtime_text}"
        "// BEGIN RTS_NATIVE_RECEIPT_WIDE_INPUTS_PRIVATE"
        "static bool deriveNativeReceiptTraceCapacities(")
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/performance-receipt-wide-path")
    file(MAKE_DIRECTORY "${_out}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptNativeWideInputs.inc" CONTENT "${_wide_inputs}")
    target_sources(${target} PRIVATE
        "${CMAKE_SOURCE_DIR}/Core/Tools/Stage5PerformanceFixtureTest/PerformanceReceiptWidePathTitleTest.cpp")
    target_include_directories(${target} PRIVATE "${_out}")
    add_test(NAME ${test_name} COMMAND ${target} --performance-receipt-wide-path)
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 30 WORKING_DIRECTORY "${_out}")
endfunction()
