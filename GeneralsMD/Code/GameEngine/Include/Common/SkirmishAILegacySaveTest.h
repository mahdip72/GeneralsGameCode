/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#pragma once

#include "Lib/BaseType.h"

// Opt-in, snapshot-only smoke probe. The caller supplies a leaf .sav name;
// the active user-data profile must provide the isolated Save directory.
void RequestSkirmishAILegacySaveTest(const char *basename);
Bool IsSkirmishAILegacySaveTestRequested();
Bool IsSkirmishAILegacySaveTestActive();
Bool StartSkirmishAILegacySaveTest();
void UpdateSkirmishAILegacySaveTest();
Int FinalizeSkirmishAILegacySaveTest(Int engineExitCode);
