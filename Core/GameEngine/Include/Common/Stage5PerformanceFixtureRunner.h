/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#pragma once

#include "Common/Stage5PerformanceFixtureContract.h"

#if defined(_WIN64)
Bool ConfigureStage5PerformanceFixture(const rts::fixture::Request &request);
Bool IsStage5PerformanceFixtureRequested();
Bool StartStage5PerformanceFixtureRunner();
void UpdateStage5PerformanceFixtureRunner();
Int FinalizeStage5PerformanceFixtureRunner(Int engineExitCode);
#endif
