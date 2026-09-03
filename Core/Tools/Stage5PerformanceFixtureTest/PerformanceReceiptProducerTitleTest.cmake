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
    set(_owner "${CMAKE_SOURCE_DIR}/${title}/Code/GameEngine/Source/GameLogic/System/GameLogic.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_replay}" "${_owner}")
    file(READ "${_replay}" _replay_text)
    file(READ "${_owner}" _owner_text)
    string(REPLACE "\r\n" "\n" _replay_text "${_replay_text}")
    string(REPLACE "\r\n" "\n" _owner_text "${_owner_text}")

    rts_producer_test_extract(_owner_methods "${_owner_text}"
        "rts::LiveSimulationPhaseOwnerCallbacks GameLogic::makeStage5PhaseGraphCallbacks()"
        "Bool GameLogic::runOwnerIntakePhase(")
    rts_producer_test_extract(_metric_copy "${_replay_text}"
        "void appendPerformanceReceiptPhase("
        "bool resolvePerformanceReceiptTimingPath(")
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

    set(_out "${CMAKE_CURRENT_BINARY_DIR}/performance-receipt-producer")
    file(MAKE_DIRECTORY "${_out}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptProducerOwnerMethods.inc" CONTENT "${_owner_methods}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptProducerMetricCopy.inc" CONTENT "${_metric_copy}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptProducerCapture.inc" CONTENT "${_capture}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptProducerSourceHold.inc" CONTENT "${_source_hold}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptProducerJobScope.inc" CONTENT "${_job_scope}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptProducerLoop.inc" CONTENT "${_loop}")
    file(GENERATE OUTPUT "${_out}/PerformanceReceiptProducerPrintMetrics.inc"
        CONTENT "${_mode_names}\n${_slice_print}\n${_job_print}")
    target_sources(${target} PRIVATE
        "${CMAKE_SOURCE_DIR}/Core/Tools/Stage5PerformanceFixtureTest/PerformanceReceiptProducerTitleTest.cpp")
    target_include_directories(${target} PRIVATE "${_out}")
    add_test(NAME ${test_name} COMMAND ${target} --performance-receipt-producer)
    set_tests_properties(${test_name} PROPERTIES
        TIMEOUT 30 WORKING_DIRECTORY "${_out}")
endfunction()
