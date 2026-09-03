/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "GameLogic/Module/UpdateModule.h"

#include <vector>

class GameLogic;

// Owner-thread lifetime wrapper. The implementation and all modern runtime
// types remain private so this public title header stays VC6/C++98 parse-safe.
class PreparedHordeObjectComputationIsland
{
public:
	PreparedHordeObjectComputationIsland();
	~PreparedHordeObjectComputationIsland();

private:
	PreparedHordeObjectComputationIsland(
		const PreparedHordeObjectComputationIsland &);
	PreparedHordeObjectComputationIsland &operator=(
		const PreparedHordeObjectComputationIsland &);
	void *m_state;

	friend Bool PrepareHordeObjectComputationIsland(GameLogic *,
		const std::vector<UpdateModulePtr> &, UnsignedInt,
		PreparedHordeObjectComputationIsland &);
	friend Bool ConsumeHordeObjectComputationIsland(GameLogic *,
		PreparedHordeObjectComputationIsland &, UpdateModulePtr,
		UnsignedInt, UpdateSleepTime &);
};

Bool PrepareHordeObjectComputationIsland(GameLogic *logic,
	const std::vector<UpdateModulePtr> &sleepyUpdates, UnsignedInt now,
	PreparedHordeObjectComputationIsland &prepared);
Bool ConsumeHordeObjectComputationIsland(GameLogic *logic,
	PreparedHordeObjectComputationIsland &prepared, UpdateModulePtr update,
	UnsignedInt now, UpdateSleepTime &sleepLen);
void ResetHordeObjectComputationIslandForMatch();
