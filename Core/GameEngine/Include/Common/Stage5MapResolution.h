/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once
#include "Common/Stage5PerformanceFixtureContract.h"

namespace rts { namespace fixture {
enum { MapPathCapacity = 1024 };
struct ResolvedMapIdentity
{
    ResolvedMapIdentity() : profileMap(false) { logicalKey[0]=runtimePath[0]=portablePath[0]='\0'; }
    char logicalKey[MapKeyCapacity];
    char runtimePath[MapPathCapacity];
    char portablePath[MapPathCapacity];
    bool profileMap;
};

inline bool CopyMapPath(const char *source, char *target, size_t capacity)
{
    if (!source || !target || strlen(source) >= capacity) return false;
    memcpy(target, source, strlen(source)+1);
    return true;
}

inline bool SameMapPath(const char *left, const char *right)
{
    if (!left || !right) return false;
    while (*left && *right) {
        const char a = *left == '/' ? '\\' : LowerAscii(*left);
        const char b = *right == '/' ? '\\' : LowerAscii(*right);
        if (a != b) return false;
        ++left; ++right;
    }
    return *left == *right;
}

// GameInfo's existing wire format retains the directory and reconstructs its
// same-named .map file. Reject keys that cannot make that round trip exactly.
inline bool IsReplayMapKey(const char *normalized)
{
    if (!normalized || strlen(normalized) < 10) return false;
    const char *directory = normalized + 5;
    const char *separator = strchr(directory, '\\');
    if (!separator || strchr(separator+1, '\\')) return false;
    const size_t length = static_cast<size_t>(separator-directory);
    const char *leaf = separator+1;
    if (strlen(leaf) != length+4) return false;
    for (size_t i=0;i<length;++i)
        if (LowerAscii(directory[i]) != LowerAscii(leaf[i])) return false;
    return true;
}

template<class Adapter>
bool ResolveMapIdentity(const char *logicalKey, Adapter &adapter, ResolvedMapIdentity *out)
{
    if (!out) return false;
    *out = ResolvedMapIdentity();
    ResolvedMapIdentity resolved;
    if (!NormalizeMapKey(logicalKey, resolved.logicalKey) || !IsReplayMapKey(resolved.logicalKey)) return false;
    char profilePortable[MapPathCapacity], profilePath[MapPathCapacity];
    strcpy(profilePortable, "UserData\\");
    strcat(profilePortable, resolved.logicalKey);
    if (!adapter.ExpandPortable(profilePortable, profilePath, sizeof(profilePath))) return false;
    // Presence includes a staged directory or cache entry: an incomplete or
    // invalid profile map must not silently select an installed archive map.
    resolved.profileMap = adapter.ProfileCandidatePresent(profilePath);
    if (!CopyMapPath(resolved.profileMap ? profilePath : resolved.logicalKey,
            resolved.runtimePath, sizeof(resolved.runtimePath)) ||
        !adapter.MakePortable(resolved.runtimePath, resolved.portablePath, sizeof(resolved.portablePath)) ||
        !SameMapPath(resolved.portablePath, resolved.profileMap ? profilePortable : resolved.logicalKey)) return false;
    char rebound[MapPathCapacity];
    if (!adapter.ExpandPortable(resolved.portablePath, rebound, sizeof(rebound)) ||
        !SameMapPath(rebound, resolved.runtimePath)) return false;
    *out = resolved;
    return true;
}
} }

#if defined(_WIN64)
// Implemented by the shared native fixture runner TU; never referenced by the
// Win32/VC6 lanes. Uses the current GameState, MapCache and FileSystem owners.
bool ResolveStage5MapIdentity(const char *logicalKey, rts::fixture::ResolvedMapIdentity *out);
#endif
