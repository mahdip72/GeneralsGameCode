include_guard(GLOBAL)

# Compile the epoch statements from the real recorder inside the controlled
# title fixture. Missing/moved boundaries require an explicit fixture update.
function(_rts_generals_epoch_slice text_variable begin_marker end_marker output_variable)
    string(FIND "${${text_variable}}" "${begin_marker}" _begin)
    if(_begin LESS 0)
        message(FATAL_ERROR "Generals recorder epoch fixture is missing: ${begin_marker}")
    endif()
    string(SUBSTRING "${${text_variable}}" ${_begin} -1 _tail)
    string(FIND "${_tail}" "${end_marker}" _end)
    if(_end LESS 1)
        message(FATAL_ERROR "Generals recorder epoch fixture has no closing boundary: ${end_marker}")
    endif()
    string(SUBSTRING "${_tail}" 0 ${_end} _slice)
    set(${output_variable} "${_slice}" PARENT_SCOPE)
endfunction()

function(rts_add_generals_pathfinding_recorder_epoch_test target)
    set(_recorder_path "${CMAKE_SOURCE_DIR}/Generals/Code/GameEngine/Source/Common/Recorder.cpp")
    set(_logic_path "${CMAKE_SOURCE_DIR}/Generals/Code/GameEngine/Source/GameLogic/System/GameLogic.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_recorder_path}" "${_logic_path}")
    file(READ "${_recorder_path}" _recorder)
    file(READ "${_logic_path}" _logic)
    string(REPLACE "\r\n" "\n" _recorder "${_recorder}")
    string(REPLACE "\r\n" "\n" _logic "${_logic}")

    _rts_generals_epoch_slice(_recorder "void RecorderClass::init() {"
        "void RecorderClass::reset()" _init)
    _rts_generals_epoch_slice(_init "m_skirmishAIReplayEpoch ="
        "OptionPreferences optionPref;" _init_epoch)
    _rts_generals_epoch_slice(_recorder
        "void RecorderClass::startRecording(GameDifficulty diff, Int originalGameMode, Int rankPoints, Int maxFPS) {"
        "Bool RecorderClass::playbackFile(AsciiString filename)" _recording)
    _rts_generals_epoch_slice(_recording
        "UnicodeString versionString = TheVersion->getUnicodeVersion();"
        "UnsignedInt versionNumber =" _recording_epoch)
    string(FIND "${_recorder}" "Bool RecorderClass::playbackFile(AsciiString filename)" _playback_start)
    if(_playback_start LESS 0)
        message(FATAL_ERROR "Generals recorder epoch fixture has no playback entry")
    endif()
    string(SUBSTRING "${_recorder}" ${_playback_start} -1 _playback)
    _rts_generals_epoch_slice(_playback "m_replayReadError = FALSE;"
        "if (!m_doingAnalysis)" _playback_reset)
    _rts_generals_epoch_slice(_playback
        "m_skirmishAIReplayEpoch = GetSkirmishAIReplayEpoch("
        "#ifdef DEBUG_CRASHING" _playback_header)
    _rts_generals_epoch_slice(_playback "Int difficulty = 0;"
        "#ifdef DEBUG_LOGGING" _playback_launch)

    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/GeneralsRecorderInitEpochUnderTest.inc"
        CONTENT "${_init_epoch}\n")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/GeneralsRecorderRecordingEpochUnderTest.inc"
        CONTENT "${_recording_epoch}\n")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/GeneralsRecorderPlaybackResetUnderTest.inc"
        CONTENT "${_playback_reset}\n")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/GeneralsRecorderPlaybackHeaderEpochUnderTest.inc"
        CONTENT "${_playback_header}\n")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/GeneralsRecorderPlaybackLaunchEpochUnderTest.inc"
        CONTENT "${_playback_launch}\n")

    # Stage 5 performs command intake in its owner phase. Recording must still
    # observe MSG_NEW_GAME before the command starts loading the map.
    _rts_generals_epoch_slice(_logic
        "Bool GameLogic::runOwnerIntakePhase( UnsignedInt &now )"
        "void GameLogic::runLegacyMutableIslandPhase" _intake)
    string(FIND "${_intake}" "TheRecorder->UPDATE();" _recorder_update)
    string(FIND "${_intake}" "processCommandList( TheCommandList );" _command_update)
    if(_recorder_update LESS 0 OR _command_update LESS 0 OR
            NOT _recorder_update LESS _command_update)
        message(FATAL_ERROR "Generals recorder must precede command processing in owner intake")
    endif()
    _rts_generals_epoch_slice(_logic "void GameLogic::startNewGame( Bool loadingSaveGame )"
        "void GameLogic::tryStartNewGame( Bool loadingSaveGame )" _new_game_wrapper)
    string(FIND "${_new_game_wrapper}" "tryStartNewGame(loadingSaveGame);" _try_call)
    _rts_generals_epoch_slice(_logic "void GameLogic::tryStartNewGame( Bool loadingSaveGame )"
        "void GameLogic::loadMapINI(" _new_game)
    string(FIND "${_new_game}" "TheAI->pathfinder()->newMap();" _path_new_map)
    if(_try_call LESS 0 OR _path_new_map LESS 0)
        message(FATAL_ERROR "Generals map startup must retain its pathfinder new-map boundary")
    endif()

    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME g_generals_pathfinding_recorder_epoch_tests
        COMMAND ${target} --generals-pathfinding-recorder-epoch)
    set_tests_properties(g_generals_pathfinding_recorder_epoch_tests PROPERTIES TIMEOUT 30)
endfunction()
