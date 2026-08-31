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
		, m_control(_controlfp(0, 0))
#endif
	{}
	void apply() const
	{
#if !defined(_WIN64)
		_controlfp(m_control, _MCW_PC | _MCW_RC | _MCW_EM);
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
