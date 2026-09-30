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

// Owner-side admission only: an admitted task may publish, retry, or fail,
// but always consumes one slot. It is never interrupted after admission.
class TextureLoadUpdateBudget
{
public:
	enum { MAXIMUM_TASKS = 8, SLICE_MILLISECONDS = 4 };
	enum Queue { NONE, RESOURCE, FOREGROUND };

	TextureLoadUpdateBudget(unsigned int start, bool &foregroundNext, bool timed = true)
		: m_start(start), m_processed(0), m_foregroundNext(foregroundNext), m_timed(timed) {}

	Queue Next(bool resourceReady, bool foregroundReady, unsigned int now)
	{
		// Always allow one task to make progress, even if owner admission took
		// the slice. Unsigned subtraction preserves the 32-bit tick rollover.
		if (m_processed >= MAXIMUM_TASKS ||
			(m_timed && m_processed != 0 && now - m_start >= SLICE_MILLISECONDS) ||
			(!resourceReady && !foregroundReady)) return NONE;
		const Queue queue = foregroundReady &&
			(!resourceReady || m_foregroundNext) ? FOREGROUND : RESOURCE;
		++m_processed;
		// Keep preference across Updates, including a single slow task, so
		// neither independent queue can starve during sustained ready bursts.
		m_foregroundNext = queue == RESOURCE;
		return queue;
	}

private:
	TextureLoadUpdateBudget(const TextureLoadUpdateBudget &);
	TextureLoadUpdateBudget &operator=(const TextureLoadUpdateBudget &);
	unsigned int m_start;
	unsigned int m_processed;
	bool &m_foregroundNext;
	bool m_timed;
};
