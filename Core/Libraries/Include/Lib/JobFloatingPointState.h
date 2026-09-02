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

#if defined(_WIN32) && !defined(_WIN64) && \
	(!defined(_MSC_VER) || _MSC_VER >= 1300)
#if defined(_MSC_VER) && _MSC_VER >= 1400 && defined(_M_IX86)
#define RTS_JOB_FLOATING_POINT_SEPARATE_X87_API 1
#elif (defined(_MSC_VER) && _MSC_VER >= 1300 && defined(_M_IX86)) || \
	(defined(__GNUC__) && defined(__i386__))
#define RTS_JOB_FLOATING_POINT_DIRECT_X87 1
#else
#error A per-unit x87 floating-point state implementation is required on Win32.
#endif
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
#if defined(RTS_JOB_FLOATING_POINT_SEPARATE_X87_API)
		// _controlfp merges x87 and SSE2 state and marks the result ambiguous
		// when their rounding modes differ. Capture the owner x87 word alone.
		unsigned sse2Control = 0;
		__control87_2(0, 0, &m_control, &sse2Control);
#else
		m_control = captureDirectX87ControlWord();
#endif
#endif
	}
	void apply() const
	{
#if !defined(_WIN64)
#if defined(RTS_JOB_FLOATING_POINT_SEPARATE_X87_API)
		// Restore only x87 here; the complete SSE2 state follows below.
		unsigned ignoredControl = 0;
		__control87_2(m_control, _MCW_PC | _MCW_RC | _MCW_EM | _MCW_IC,
			&ignoredControl, 0);
#else
		applyDirectX87ControlWord(m_control);
#endif
#endif
		_mm_setcsr(m_mxcsr);
	}
private:
#if defined(RTS_JOB_FLOATING_POINT_DIRECT_X87)
	static unsigned short captureDirectX87ControlWord()
	{
		unsigned short controlWord = 0;
#if defined(_MSC_VER)
		__asm { fnstcw [controlWord] }
#else
		__asm__ __volatile__("fnstcw %0" : "=m"(controlWord));
#endif
		return controlWord;
	}
	static void applyDirectX87ControlWord(unsigned short controlWord)
	{
#if defined(_MSC_VER)
		__asm { fldcw [controlWord] }
#else
		__asm__ __volatile__("fldcw %0" : : "m"(controlWord));
#endif
	}
#endif
	unsigned m_mxcsr;
#if !defined(_WIN64)
#if defined(RTS_JOB_FLOATING_POINT_SEPARATE_X87_API)
	unsigned m_control;
#else
	unsigned short m_control;
#endif
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

#if defined(RTS_JOB_FLOATING_POINT_SEPARATE_X87_API)
#undef RTS_JOB_FLOATING_POINT_SEPARATE_X87_API
#endif
#if defined(RTS_JOB_FLOATING_POINT_DIRECT_X87)
#undef RTS_JOB_FLOATING_POINT_DIRECT_X87
#endif
