/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#if defined(_WIN32) && (!defined(_MSC_VER) || _MSC_VER >= 1300)
#include <float.h>
#include <xmmintrin.h>
#endif

namespace rts
{
// Capture this numeric state on the submitting owner. A worker's previous
// kernel, or its default thread state, must not change a scalar kernel's result.
// VC6 jobs execute inline on their owner and require no state transition.
class JobFloatingPointState
{
public:
#if defined(_WIN32) && (!defined(_MSC_VER) || _MSC_VER >= 1300)
	JobFloatingPointState() : m_mxcsr(_mm_getcsr())
#if !defined(_WIN64)
		, m_control(0)
#endif
	{
#if !defined(_WIN64)
#if defined(_MSC_VER) && _MSC_VER >= 1400 && defined(_M_IX86)
		// _controlfp merges x87 and SSE2 state and marks the result ambiguous
		// when their rounding modes differ. Capture the owner x87 word alone.
		unsigned sse2Control = 0;
		__control87_2(0, 0, &m_control, &sse2Control);
#else
		m_control = _controlfp(0, 0);
#endif
#endif
	}
	void apply() const
	{
#if !defined(_WIN64)
#if defined(_MSC_VER) && _MSC_VER >= 1400 && defined(_M_IX86)
		// Restore only x87 here; the complete SSE2 state follows below.
		unsigned ignoredControl = 0;
		__control87_2(m_control, _MCW_PC | _MCW_RC | _MCW_EM,
			&ignoredControl, 0);
#else
		_controlfp(m_control, _MCW_PC | _MCW_RC | _MCW_EM);
#endif
#endif
		_mm_setcsr(m_mxcsr);
	}
private:
	unsigned m_mxcsr;
#if !defined(_WIN64)
	unsigned m_control;
#endif
#else
	JobFloatingPointState() {}
	void apply() const {}
#endif
};

class JobFloatingPointScope
{
public:
	explicit JobFloatingPointScope(const JobFloatingPointState &state) { state.apply(); }
	~JobFloatingPointScope() { m_previous.apply(); }
private:
	JobFloatingPointScope(const JobFloatingPointScope &);
	JobFloatingPointScope &operator=(const JobFloatingPointScope &);
	JobFloatingPointState m_previous;
};
}
