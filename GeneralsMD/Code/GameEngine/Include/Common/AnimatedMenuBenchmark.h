#pragma once
#include "Lib/AnimatedMenuBenchmarkOptions.h"
#if defined(_WIN64) && RTS_ZEROHOUR
#include "Common/FileSystem.h"
#include "Common/FramePacer.h"
#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "Common/crc.h"
#include "GameClient/Shell.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/View.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/TerrainLogic.h"
#include "Lib/ValidationProfileRoot.h"
#include "Renderer/RenderGameClient.h"
#include <windows.h>
#include <share.h>
#include <stdio.h>
#include <string>

namespace rts { namespace animated_menu {
struct Capture {
    Capture() : file(NULL), frequency(0), started(0), ready(0), phase(0), complete(false),
        failed(false), oldLogicEnabled(false), oldLogicLimit(0), mapCRC(0) {}
    FILE *file;
    LONGLONG frequency, started, ready;
    int phase;
    bool complete, failed, oldLogicEnabled;
    int oldLogicLimit;
    unsigned int mapCRC;
    std::string map;
};
inline Capture &ProcessCapture() { static Capture capture; return capture; }
inline LONGLONG Now() {
    LARGE_INTEGER value;
    return QueryPerformanceCounter(&value) ? value.QuadPart : 0;
}
inline void Fail(const char *reason) {
    Capture &c = ProcessCapture(); c.failed = true;
    printf("ANIMATED_MENU_BENCHMARK_FAIL reason=%s\n", reason); fflush(stdout);
    TheGameEngine->setQuitting(TRUE);
}
inline bool Start() {
    if (!ProcessOptions().requested) return true;
    Capture &c = ProcessCapture();
    char profile[MAX_PATH], path[MAX_PATH + 80]; LARGE_INTEGER frequency;
    if (validation::ReadProcessLocalProfileRoot(profile, MAX_PATH) != validation::PROCESS_LOCAL_PROFILE_ROOT_VALID ||
        !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 || !(c.started = Now())) {
        Fail("invalid_profile_or_clock"); return false;
    }
    c.frequency = frequency.QuadPart;
    _snprintf(path, sizeof(path), "%sAnimatedMenuBenchmark-markers-%lu.csv", profile, GetCurrentProcessId());
    path[sizeof(path)-1] = '\0';
    // Exclusive create prevents replacement of stale evidence. Only this process writes.
    c.file = _fsopen(path, "w+x", _SH_DENYWR);
    if (!c.file) { Fail("marker_create_failed"); return false; }
    if (fputs("row_type,process_id,title,marker_qpc,observed_qpc,qpc_frequency,logic_frame,game_mode,shell_active,main_menu_visible,shell_map_hex,shell_map_crc,requested_cap_fps,preference_cap_enabled,limiter_enabled,actual_cap_enabled,effective_cap_fps,effective_cap_raw,logic_scaling_enabled,logic_limit_fps,actual_logic_fps,time_multiplier,time_fast,tivo_fast,paused,time_frozen,game_halted,replay,headless,windowed,renderer,width,height,terrain_lod,dynamic_lod\n", c.file) < 0 || fflush(c.file) != 0) {
        Fail("marker_header_failed"); return false;
    }
    c.oldLogicEnabled = TheFramePacer->isLogicTimeScaleEnabled() != FALSE;
    c.oldLogicLimit = TheFramePacer->getLogicTimeScaleFps();
    // Only this diagnostic lane decouples render opportunity from normal 30Hz logic.
    TheFramePacer->setLogicTimeScaleFps(LOGICFRAMES_PER_SECOND);
    TheFramePacer->enableLogicTimeScale(TRUE);
    ProcessOptions().active = true;
    TheFramePacer->reset();
    return true;
}
inline bool ShellReady() {
    WindowLayout *top = TheShell ? TheShell->top() : NULL;
    return TheGameLogic && TheGameLogic->getGameMode() == GAME_SHELL &&
        !TheGameLogic->isLoadingMap() && !TheGameLogic->isLoadingSave() &&
        TheShell && TheShell->isShellActive() && top && !top->isHidden() &&
        _stricmp(top->getFilename().str(), "Menus/MainMenu.wnd") == 0 &&
        TheTerrainLogic && TheTerrainLogic->getSourceFilename().isNotEmpty() &&
        _stricmp(TheTerrainLogic->getSourceFilename().str(), TheGlobalData->m_shellMapName.str()) == 0 &&
        _stricmp(TheTerrainLogic->getSourceFilename().str(), TheGlobalData->m_mapName.str()) == 0;
}
inline bool NormalLogic() {
    return TheTacticalView && TheScriptEngine &&
        TheFramePacer->isLogicTimeScaleEnabled() &&
        TheFramePacer->getLogicTimeScaleFps() == LOGICFRAMES_PER_SECOND &&
        TheFramePacer->getActualLogicTimeScaleFps() == LOGICFRAMES_PER_SECOND &&
        TheTacticalView->getTimeMultiplier() == 1 && !TheScriptEngine->isTimeFast() &&
        !TheGlobalData->m_TiVOFastMode && !TheGameLogic->isGamePaused() &&
        !TheFramePacer->isTimeFrozen() && !TheFramePacer->isGameHalted() &&
        !TheGameLogic->isInReplayGame() && !TheGlobalData->m_headless;
}
inline bool ReadMap() {
    Capture &c = ProcessCapture(); c.map = TheTerrainLogic->getSourceFilename().str();
    File *file = TheFileSystem ? TheFileSystem->openFile(c.map.c_str(), File::READ|File::BINARY|File::STREAMING) : NULL;
    if (!file) return false;
    int length = file->size(), offset = 0; CRC crc; unsigned char bytes[16384];
    bool valid = length > 0 && length <= 64*1024*1024;
    while (valid && offset < length) {
        int wanted = length-offset < int(sizeof(bytes)) ? length-offset : int(sizeof(bytes));
        int count = file->read(bytes, wanted);
        if (count <= 0 || count > wanted) { valid = false; break; }
        crc.computeCRC(bytes, count); offset += count;
    }
    unsigned char extra;
    valid = valid && offset == length && file->read(&extra, 1) == 0;
    file->close();
    if (valid) c.mapCRC = crc.get();
    return valid;
}
inline bool Row(const char *name, LONGLONG target, LONGLONG observed) {
    Capture &c = ProcessCapture();
    int width=0, height=0, bits=0; bool windowed=false;
    const bool native = render::IsNativeGameRendererActive();
    if (!ShellReady() || !NormalLogic() || !native || render::GetGameRendererResolution(&width,&height,&bits,&windowed) != render::RENDER_RESULT_OK)
        return false;
    const bool capped = TheFramePacer->isActualFramesPerSecondLimitEnabled() != FALSE;
    const int raw = TheFramePacer->getActualFramesPerSecondLimit();
    std::string hex; const char *digits="0123456789ABCDEF";
    for (size_t i=0;i<c.map.size();++i) { unsigned char b=static_cast<unsigned char>(c.map[i]); hex += digits[b>>4]; hex += digits[b&15]; }
    int result = fprintf(c.file,
        "%s,%lu,zero_hour,%lld,%lld,%lld,%u,%d,%d,%d,%s,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.9g,%d,%d,%d,%d,%d,%d,%d,%d,%s,%d,%d,%d,%d\n",
        name,GetCurrentProcessId(),target,observed,c.frequency,TheGameLogic->getFrame(),int(TheGameLogic->getGameMode()),
        int(TheShell->isShellActive()),int(!TheShell->top()->isHidden()),hex.c_str(),c.mapCRC,
        TheFramePacer->getFramesPerSecondLimit(),int(TheGlobalData->m_useFpsLimit),int(TheFramePacer->isFramesPerSecondLimitEnabled()),
        int(capped),capped?raw:0,raw,int(TheFramePacer->isLogicTimeScaleEnabled()),TheFramePacer->getLogicTimeScaleFps(),
        TheFramePacer->getActualLogicTimeScaleFps(),double(TheTacticalView->getTimeMultiplier()),int(TheScriptEngine->isTimeFast()),
        int(TheGlobalData->m_TiVOFastMode),int(TheGameLogic->isGamePaused()),int(TheFramePacer->isTimeFrozen()),
        int(TheFramePacer->isGameHalted()),int(TheGameLogic->isInReplayGame()),int(TheGlobalData->m_headless),int(windowed),
        native?"d3d11":"dx8",width,height,int(TheGlobalData->m_terrainLOD),int(TheGlobalData->m_enableDynamicLOD));
    return result > 0 && fflush(c.file) == 0;
}
inline void ObserveCompletedFrame() {
    if (!ProcessOptions().active) return;
    Capture &c=ProcessCapture(); LONGLONG now=Now();
    if (now <= 0) { Fail("clock_failed"); return; }
    if (!c.ready) {
        if (now-c.started > 30*c.frequency) { Fail("shell_ready_timeout"); return; }
        if (!ShellReady()) return;
        // Read exact loaded virtual-filesystem bytes once, before the cold-ready timestamp.
        if (!ReadMap()) { Fail("shell_map_identity_failed"); return; }
        c.ready=Now();
        if (c.ready <= 0) { Fail("clock_failed"); return; }
        if (!Row("shell_ready",c.ready,c.ready)) { Fail("ready_marker_failed"); return; }
        c.phase=1; return;
    }
    if (!ShellReady() || !NormalLogic() || _stricmp(TheTerrainLogic->getSourceFilename().str(),c.map.c_str()) != 0) {
        Fail("shell_state_changed"); return;
    }
    LONGLONG begin=c.ready+20*c.frequency, stop=begin+30*c.frequency;
    if (c.phase==1 && now>=begin) {
        if (now-begin>c.frequency/2 || !Row("warmup_complete",begin,now) || !Row("measurement_begin",begin,now)) {
            Fail("measurement_begin_failed_or_late"); return;
        }
        c.phase=2;
    }
    if (c.phase==2 && now>=stop) {
        if (now-stop>c.frequency/2 || !Row("measurement_stop",stop,now)) { Fail("measurement_stop_failed_or_late"); return; }
        c.phase=3; c.complete=true;
    }
    // One bounded, excluded tail second permits passive post-stop window observation.
    if (c.phase==3 && now>=stop+c.frequency) TheGameEngine->setQuitting(TRUE);
}
inline int Finalize(int exitcode) {
    if (!ProcessOptions().requested) return exitcode;
    Capture &c=ProcessCapture(); bool clean=c.complete && !c.failed;
    if (c.file) { if (fclose(c.file)!=0) clean=false; c.file=NULL; }
    if (ProcessOptions().active) {
        ProcessOptions().active=false;
        TheFramePacer->setLogicTimeScaleFps(c.oldLogicLimit);
        TheFramePacer->enableLogicTimeScale(c.oldLogicEnabled?TRUE:FALSE);
    }
    printf("ANIMATED_MENU_BENCHMARK_COMPLETE complete=%d\n",clean?1:0); fflush(stdout);
    return clean ? exitcode : 2;
}
} }
#else
namespace rts { namespace animated_menu {
inline bool Start() { return !ProcessOptions().requested; }
inline void ObserveCompletedFrame() {}
inline int Finalize(int code) { return ProcessOptions().requested ? 2 : code; }
} }
#endif
