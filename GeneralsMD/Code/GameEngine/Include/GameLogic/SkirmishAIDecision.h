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

template <typename PlanType>
inline PlanType *GetSkirmishAutomaticConstructionPlan(PlanType *currentPlan, PlanType * /*previousPlan*/)
{
	return currentPlan;
}

inline int GetSupplyDefenseMemoryFrames(int logicFramesPerSecond)
{
	return 10 * logicFramesPerSecond;
}

inline bool ShouldPreferSkirmishRetaliation(bool candidateIsSkirmishAI, bool candidateTargetsThisAI)
{
	return candidateIsSkirmishAI && candidateTargetsThisAI;
}

inline bool HasSkirmishRallyOffset(float x, float y)
{
	return x > 1.0f || x < -1.0f || y > 1.0f || y < -1.0f;
}
