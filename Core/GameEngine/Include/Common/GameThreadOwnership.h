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

#pragma once

class GameThreadOwnership
{
public:
	static void AttachCurrentThread();
	static void DetachCurrentThread();
	static bool IsAttached();
	static bool IsCurrentThread();
	static void AssertCurrentThread(const char *boundary);
};

#if defined(DEBUG_CRASHING)
#define ASSERT_GAME_THREAD(boundary) GameThreadOwnership::AssertCurrentThread(boundary)
#else
#define ASSERT_GAME_THREAD(boundary) ((void)0)
#endif
