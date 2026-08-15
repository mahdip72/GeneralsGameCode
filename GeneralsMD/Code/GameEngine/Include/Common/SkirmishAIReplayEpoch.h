/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

#pragma once

#include "Common/UnicodeString.h"

inline const WideChar *GetSkirmishAILivenessReplayMarker()
{
	return L" [SkirmishAILiveness=1]";
}

inline void MarkReplayVersionForSkirmishAILivenessRecovery(UnicodeString& versionTimeString)
{
	versionTimeString.concat(GetSkirmishAILivenessReplayMarker());
}

inline Bool ReplayVersionUsesSkirmishAILivenessRecovery(const UnicodeString& versionTimeString)
{
	return versionTimeString.endsWith(GetSkirmishAILivenessReplayMarker());
}
