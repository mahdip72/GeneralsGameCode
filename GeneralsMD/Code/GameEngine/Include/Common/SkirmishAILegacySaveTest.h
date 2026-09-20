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

inline Bool IsSkirmishAILegacySaveBuilderLive(
	Bool exists, Bool destroyed, Bool effectivelyDead, Bool sold)
{
	return exists && !destroyed && !effectivelyDead && !sold;
}

struct SkirmishAILegacySaveCandidateInput
{
	SkirmishAILegacySaveCandidateInput() :
		isComputer(FALSE),
		isSkirmishAI(FALSE),
		hasPlayerTemplate(FALSE),
		hasCompletedPrimaryCenter(FALSE),
		hasOtherStructure(FALSE),
		hasBuildInfo(FALSE),
		hasScoreKeeper(FALSE),
		compatibleBuilderCount(0),
		centerCost(0),
		cash(0),
		reserveAdmitted(FALSE)
	{
	}

	Bool isComputer;
	Bool isSkirmishAI;
	Bool hasPlayerTemplate;
	Bool hasCompletedPrimaryCenter;
	Bool hasOtherStructure;
	Bool hasBuildInfo;
	Bool hasScoreKeeper;
	Int compatibleBuilderCount;
	Int centerCost;
	UnsignedInt cash;
	Bool reserveAdmitted;
};

inline Bool IsSkirmishAILegacySaveCandidateEligible(
	const SkirmishAILegacySaveCandidateInput &input)
{
	return input.isComputer && input.isSkirmishAI && input.hasPlayerTemplate &&
		input.hasCompletedPrimaryCenter && input.hasOtherStructure &&
		input.hasBuildInfo && input.hasScoreKeeper &&
		input.compatibleBuilderCount > 0 && input.centerCost > 0 &&
		input.cash >= static_cast<UnsignedInt>(input.centerCost) &&
		input.reserveAdmitted;
}

inline Bool ShouldSelectSkirmishAILegacySaveCandidate(
	Bool hasSelection, Int selectedPlayerIndex, Int candidatePlayerIndex,
	const SkirmishAILegacySaveCandidateInput &input)
{
	return candidatePlayerIndex >= 0 &&
		IsSkirmishAILegacySaveCandidateEligible(input) &&
		(!hasSelection || candidatePlayerIndex < selectedPlayerIndex);
}

// Opt-in, snapshot-only smoke probe. The caller supplies a leaf .sav name;
// the active user-data profile must provide the isolated Save directory.
void RequestSkirmishAILegacySaveTest(const char *basename);
Bool IsSkirmishAILegacySaveTestRequested();
Bool IsSkirmishAILegacySaveTestActive();
Bool StartSkirmishAILegacySaveTest();
void UpdateSkirmishAILegacySaveTest();
Int FinalizeSkirmishAILegacySaveTest(Int engineExitCode);
