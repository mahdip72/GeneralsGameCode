/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#pragma once

namespace rts
{

// C++03-safe checked transitions for an exact bounded free-record count.
// Failed transitions leave the caller's count untouched.
inline bool TryConsumeBoundedFreeCount(unsigned capacity, unsigned &freeCount)
{
	if (freeCount == 0 || freeCount > capacity)
		return false;
	--freeCount;
	return true;
}

inline bool TryRestoreBoundedFreeCount(unsigned capacity, unsigned &freeCount)
{
	if (freeCount >= capacity)
		return false;
	++freeCount;
	return true;
}

inline bool IsBoundedFreeCountValid(unsigned capacity, unsigned freeCount)
{
	return freeCount <= capacity;
}

} // namespace rts
