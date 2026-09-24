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

#include "Lib/BaseTypeCore.h"

enum
{
	SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH = 64,
	SKIRMISH_AI_TEST_RECEIPT_NONCE_LENGTH = 64,
	SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH = 1024,
	SKIRMISH_AI_TEST_RECEIPT_SCENARIO_LENGTH = 32
};

// Fields emitted by the executable after a fresh skirmish run.  These are
// deliberately fixed-width and C++98-compatible because the same runner is
// linked by the legacy and native title targets.
struct SkirmishAITestReplayReceipt
{
	Int seed;
	Int winnerTeam;
	UnsignedInt endFrame;
	Int replayEpoch;
	char scenario[SKIRMISH_AI_TEST_RECEIPT_SCENARIO_LENGTH];
	char executableSha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1];
	char replaySha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1];
	char runNonce[SKIRMISH_AI_TEST_RECEIPT_NONCE_LENGTH + 1];
	char replayPath[SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH];
};

// Copies a completed LastReplay file to a distinct destination, hashing the
// bytes that were copied.  The destination is committed with an atomic same-
// volume rename only after the complete copy and hash succeed.
Bool RetainSkirmishAITestReplayAtomically(const char *sourcePath,
	const char *destinationPath, char sha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1]);

Bool IsValidSkirmishAITestReplayReceipt(
	const SkirmishAITestReplayReceipt &receipt, Int expectedReplayEpoch);
