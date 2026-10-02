/* Command & Conquer Generals(tm) Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once
#include "Common/Stage5MapResolution.h"

namespace rts { namespace ai_fixture {
struct MapRequest
{
    bool requested;
    char mapKey[rts::fixture::MapKeyCapacity];
    char sha256[65];
    unsigned byteCount, crc;
    MapRequest() : requested(false), byteCount(0), crc(0) { mapKey[0] = sha256[0] = 0; }
};

inline bool UpperHex(const char *text, unsigned length)
{
    if (!text || strlen(text) != length) return false;
    for (unsigned i = 0; i < length; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'A' && text[i] <= 'F'))) return false;
    return true;
}

// Validate both command-line passes before any mode handler can act. Optional
// absence preserves old title/VC6/default CLI behavior exactly.
inline bool ParseMapRequest(int argc, const char *const *argv, bool native,
    MapRequest *request, const char **error)
{
    if (!request || argc < 0 || (argc && !argv)) return rts::fixture::Fail(error, "invalid_arguments");
    *request = MapRequest();
    if (error) *error = 0;
    int option = -1, scenarios = 0;
    bool conflict = false;
    for (int i = 1; i < argc; ++i)
    {
        if (rts::fixture::SameToken(argv[i], "-skirmishAITestReviewedMap"))
        {
            if (option != -1) return rts::fixture::Fail(error, "duplicate_reviewed_map");
            option = i;
        }
        if (rts::fixture::SameToken(argv[i], "-runSkirmishAITest4v2")) ++scenarios;
        if (!rts::fixture::SameToken(argv[i], "-runSkirmishAITest4v2") &&
            (rts::fixture::IsConflictingOption(argv[i]) ||
             rts::fixture::SameToken(argv[i], "-runStage5PerformanceFixture"))) conflict = true;
    }
    if (option == -1) return true;
    if (!native || conflict || scenarios != 1) return rts::fixture::Fail(error, "reviewed_map_requires_native_4v2");
    if (argc - option < 5) return rts::fixture::Fail(error, "missing_reviewed_map_arguments");
    MapRequest parsed;
    if (!rts::fixture::NormalizeMapKey(argv[option + 1], parsed.mapKey) ||
        !rts::fixture::IsReplayMapKey(parsed.mapKey) ||
        rts::fixture::SameToken(parsed.mapKey, "Maps\\Twilight Flame\\Twilight Flame.map"))
        return rts::fixture::Fail(error, "invalid_reviewed_map_key");
    if (!UpperHex(argv[option + 2], 64) || !UpperHex(argv[option + 4], 8) ||
        !rts::fixture::ParsePositive(argv[option + 3], 64U * 1024U * 1024U, &parsed.byteCount) ||
        parsed.byteCount < 16384U) return rts::fixture::Fail(error, "invalid_reviewed_map_identity");
    memcpy(parsed.sha256, argv[option + 2], 65);
    for (unsigned j = 0; j < 8; ++j)
    {
        const char value = argv[option + 4][j];
        parsed.crc = (parsed.crc << 4) + static_cast<unsigned>(value <= '9' ? value - '0' : value - 'A' + 10);
    }
    parsed.requested = true;
    *request = parsed;
    return true;
}
} }
