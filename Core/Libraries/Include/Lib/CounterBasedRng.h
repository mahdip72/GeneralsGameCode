/*
** Command & Conquer Generals(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#pragma once

#include <Utility/stdint_adapter.h>

namespace rts
{
enum AICounterRngAlgorithm
{
	AI_COUNTER_RNG_ARX32_V1 = 1
};

enum AICounterRngDomain
{
	AI_COUNTER_RNG_DOMAIN_PLAYER_PLANNING = 1
};

enum AICounterRngEvent
{
	AI_COUNTER_RNG_EVENT_PRODUCTION_TIE = 1,
	AI_COUNTER_RNG_EVENT_ENEMY_TIE = 2
};

// A semantic, replay-stable random key. Every field is an integer identity;
// addresses, thread IDs, container layout, and source-code locations are
// deliberately excluded. Increment drawOrdinal for multiple draws by one
// logical event instead of retaining mutable generator state.
struct AICounterRngKey
{
	uint32_t simulationEpoch;
	uint32_t matchSeed;
	uint32_t frame;
	uint32_t domain;
	uint32_t playerIndex;
	uint32_t ownerStableId;
	uint32_t sourceStableId;
	uint32_t eventKind;
	uint32_t eventOrdinal;
	uint32_t drawOrdinal;
};

struct AICounterRngLedgerRecord
{
	AICounterRngKey key;
	uint32_t algorithm;
	uint32_t rawValue;
	uint32_t exclusiveUpperBound;
	uint32_t selectedIndex;
	uint32_t valid;
};

void ClearAICounterRngKey(AICounterRngKey *key);
void ClearAICounterRngLedgerRecord(AICounterRngLedgerRecord *record);
AICounterRngKey AdvanceAICounterRngDraw(const AICounterRngKey &key, uint32_t amount);
uint32_t GenerateAICounterRngWord(const AICounterRngKey &key);

// Returns false for an empty range. The bounded result uses a fixed modulo
// mapping because this lane is intended for small deterministic selections,
// not simulation-global random streams.
bool GenerateAICounterRngIndex(const AICounterRngKey &key,
	uint32_t exclusiveUpperBound, uint32_t *selectedIndex,
	AICounterRngLedgerRecord *record);

bool EqualAICounterRngKey(const AICounterRngKey &left, const AICounterRngKey &right);
bool EqualAICounterRngLedgerRecord(const AICounterRngLedgerRecord &left,
	const AICounterRngLedgerRecord &right);
}
