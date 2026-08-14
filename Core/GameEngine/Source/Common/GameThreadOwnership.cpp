/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "PreRTS.h"

#include "Common/GameThreadOwnership.h"

#if !defined(_UNIX)

#include <windows.h>

#include "Common/Debug.h"

static DWORD s_gameThreadId = 0;

void GameThreadOwnership::AttachCurrentThread()
{
	s_gameThreadId = GetCurrentThreadId();
}

void GameThreadOwnership::DetachCurrentThread()
{
	if (s_gameThreadId == GetCurrentThreadId())
	{
		s_gameThreadId = 0;
	}
}

bool GameThreadOwnership::IsAttached()
{
	return s_gameThreadId != 0;
}

bool GameThreadOwnership::IsCurrentThread()
{
	return s_gameThreadId != 0 && s_gameThreadId == GetCurrentThreadId();
}

void GameThreadOwnership::AssertCurrentThread(const char *boundary)
{
	if (s_gameThreadId != 0 && s_gameThreadId != GetCurrentThreadId())
	{
		DEBUG_CRASH(("Game thread ownership violation at %s", boundary));
	}
}

#else

void GameThreadOwnership::AttachCurrentThread()
{
}

void GameThreadOwnership::DetachCurrentThread()
{
}

bool GameThreadOwnership::IsAttached()
{
	return false;
}

bool GameThreadOwnership::IsCurrentThread()
{
	return false;
}

void GameThreadOwnership::AssertCurrentThread(const char *boundary)
{
	((void)boundary);
}

#endif
