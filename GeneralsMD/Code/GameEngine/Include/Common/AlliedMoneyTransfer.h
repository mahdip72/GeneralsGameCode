#pragma once

#include "Common/GameType.h"
#include <limits.h>

// Pure, C++98-compatible checks shared by the human command and AI transfer.
namespace AlliedMoneyTransfer
{
enum
{
	MIN_HUMAN_AMOUNT = 100,
	MAX_HUMAN_AMOUNT = 10000,
	HUMAN_AMOUNT_STEP = 100
};

inline Bool IsHumanAmountValid(Int amount)
{
	return amount >= MIN_HUMAN_AMOUNT && amount <= MAX_HUMAN_AMOUNT &&
		amount % HUMAN_AMOUNT_STEP == 0;
}

inline Bool CanTransfer(Int amount, UnsignedInt donorCash, UnsignedInt recipientCash,
	Bool distinctPlayers, Bool donorActive, Bool recipientActive, Bool mutuallyAllied)
{
	return amount > 0 && distinctPlayers && donorActive && recipientActive && mutuallyAllied &&
		static_cast<UnsignedInt>(amount) <= donorCash &&
		static_cast<UnsignedInt>(amount) <= UINT_MAX - recipientCash;
}
}
