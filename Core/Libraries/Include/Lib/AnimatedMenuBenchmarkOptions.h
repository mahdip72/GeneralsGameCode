#pragma once

#include <string.h>
namespace rts { namespace animated_menu {
struct Options {
    Options() : requested(false), active(false) {}
    bool requested, active;
};
inline Options &ProcessOptions() { static Options options; return options; }
// Validation precedes both parser passes; ordering cannot conceal a conflict.
inline bool Configure(int argc, const char *const *argv, bool supported, const char **error) {
    int requests = 0; bool conflict = false;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (_stricmp(a, "-runAnimatedMenuBenchmark") == 0) ++requests;
        else if (_strnicmp(a, "-run", 4) == 0 ||
            _strnicmp(a, "-renderBenchmark", 16) == 0 ||
            _strnicmp(a, "-renderVisual", 13) == 0 ||
            _strnicmp(a, "-skirmishAI", 11) == 0 ||
            _stricmp(a, "-headless") == 0 || _stricmp(a, "-replay") == 0 ||
            _stricmp(a, "-loadsave") == 0 || _stricmp(a, "-map") == 0 ||
            _stricmp(a, "-file") == 0 || _stricmp(a, "-shellmap") == 0 ||
            _stricmp(a, "-benchmark") == 0 || _stricmp(a, "-buildmapcache") == 0 ||
            _stricmp(a, "-mod") == 0 || _stricmp(a, "-noFPSLimit") == 0 ||
            _stricmp(a, "-fps") == 0 || _stricmp(a, "-noshellmap") == 0 ||
            _stricmp(a, "-noShellAnim") == 0 || _stricmp(a, "-noshaders") == 0 ||
            _stricmp(a, "-particleEdit") == 0) conflict = true;
    }
    if (requests && (!supported || requests != 1 || conflict)) {
        if (error) *error = !supported ? "unsupported_title_or_architecture" : "conflicting_option";
        return false;
    }
    ProcessOptions().requested = requests != 0;
    return true;
}
} }
