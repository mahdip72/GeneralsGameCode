#include "Lib/CounterBasedRng.h"

#include <string.h>

namespace rts
{
namespace
{
uint32_t RotateLeft32(uint32_t value, unsigned distance)
{
	return (value << distance) | (value >> (32U - distance));
}

uint32_t MixAICounterRngWord(uint32_t state, uint32_t word, uint32_t ordinal)
{
	state += word + 0x9e3779b9U + ordinal * 0x85ebca6bU;
	state ^= RotateLeft32(state, 13U);
	state *= 0xc2b2ae35U;
	state ^= state >> 16U;
	return state;
}

uint32_t FinalizeAICounterRngWord(uint32_t value)
{
	value ^= value >> 16U;
	value *= 0x7feb352dU;
	value ^= value >> 15U;
	value *= 0x846ca68bU;
	value ^= value >> 16U;
	return value;
}
}

void ClearAICounterRngKey(AICounterRngKey *key)
{
	if (key != 0)
		memset(key, 0, sizeof(*key));
}

void ClearAICounterRngLedgerRecord(AICounterRngLedgerRecord *record)
{
	if (record != 0)
		memset(record, 0, sizeof(*record));
}

AICounterRngKey AdvanceAICounterRngDraw(const AICounterRngKey &key, uint32_t amount)
{
	AICounterRngKey result = key;
	result.drawOrdinal += amount;
	return result;
}

uint32_t GenerateAICounterRngWord(const AICounterRngKey &key)
{
	uint32_t state = 0x243f6a88U;
	state = MixAICounterRngWord(state, key.simulationEpoch, 0U);
	state = MixAICounterRngWord(state, key.matchSeed, 1U);
	state = MixAICounterRngWord(state, key.frame, 2U);
	state = MixAICounterRngWord(state, key.domain, 3U);
	state = MixAICounterRngWord(state, key.playerIndex, 4U);
	state = MixAICounterRngWord(state, key.ownerStableId, 5U);
	state = MixAICounterRngWord(state, key.sourceStableId, 6U);
	state = MixAICounterRngWord(state, key.eventKind, 7U);
	state = MixAICounterRngWord(state, key.eventOrdinal, 8U);
	state = MixAICounterRngWord(state, key.drawOrdinal, 9U);
	return FinalizeAICounterRngWord(state ^ 0x13198a2eU);
}

bool GenerateAICounterRngIndex(const AICounterRngKey &key,
	uint32_t exclusiveUpperBound, uint32_t *selectedIndex,
	AICounterRngLedgerRecord *record)
{
	if (selectedIndex == 0 || exclusiveUpperBound == 0U)
	{
		ClearAICounterRngLedgerRecord(record);
		return false;
	}

	const uint32_t rawValue = GenerateAICounterRngWord(key);
	const uint32_t result = rawValue % exclusiveUpperBound;
	*selectedIndex = result;
	if (record != 0)
	{
		record->key = key;
		record->algorithm = AI_COUNTER_RNG_ARX32_V1;
		record->rawValue = rawValue;
		record->exclusiveUpperBound = exclusiveUpperBound;
		record->selectedIndex = result;
		record->valid = 1U;
	}
	return true;
}

bool EqualAICounterRngKey(const AICounterRngKey &left, const AICounterRngKey &right)
{
	return left.simulationEpoch == right.simulationEpoch &&
		left.matchSeed == right.matchSeed &&
		left.frame == right.frame &&
		left.domain == right.domain &&
		left.playerIndex == right.playerIndex &&
		left.ownerStableId == right.ownerStableId &&
		left.sourceStableId == right.sourceStableId &&
		left.eventKind == right.eventKind &&
		left.eventOrdinal == right.eventOrdinal &&
		left.drawOrdinal == right.drawOrdinal;
}

bool EqualAICounterRngLedgerRecord(const AICounterRngLedgerRecord &left,
	const AICounterRngLedgerRecord &right)
{
	return EqualAICounterRngKey(left.key, right.key) &&
		left.algorithm == right.algorithm &&
		left.rawValue == right.rawValue &&
		left.exclusiveUpperBound == right.exclusiveUpperBound &&
		left.selectedIndex == right.selectedIndex &&
		left.valid == right.valid;
}
}
