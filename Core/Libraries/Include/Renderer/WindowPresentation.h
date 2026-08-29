#pragma once

#ifndef RTS_RENDERER_WINDOW_PRESENTATION_H
#define RTS_RENDERER_WINDOW_PRESENTATION_H

#include <windows.h>

namespace rts
{
namespace render
{

// This policy is deliberately independent of the renderer implementation.  The
// D3D11 path presents through a normal HWND swap chain, so fullscreen is a
// borderless window transition rather than a DXGI exclusive-mode transition.
enum WindowPresentationKind
{
	WINDOW_PRESENTATION_WINDOWED,
	WINDOW_PRESENTATION_BORDERLESS_FULLSCREEN,
	WINDOW_PRESENTATION_LEGACY_EXCLUSIVE_FULLSCREEN
};

struct WindowPresentationPlan
{
	WindowPresentationPlan() :
		kind(WINDOW_PRESENTATION_WINDOWED),
		style(0),
		exStyle(0),
		usesExclusiveMode(false)
	{
		rect.left = 0;
		rect.top = 0;
		rect.right = 0;
		rect.bottom = 0;
	}

	WindowPresentationKind kind;
	DWORD style;
	DWORD exStyle;
	RECT rect;
	bool usesExclusiveMode;
};

inline bool IsValidWindowPresentationRect(const RECT &rect)
{
	return rect.right > rect.left && rect.bottom > rect.top;
}

inline bool EqualWindowPresentationRect(const RECT &left, const RECT &right)
{
	return left.left == right.left && left.top == right.top &&
		left.right == right.right && left.bottom == right.bottom;
}

inline DWORD BorderlessWindowStyle(DWORD savedStyle)
{
	// Keep only flags that affect child clipping/visibility.  Caption,
	// sizing, border, and system-menu bits must not survive the transition.
	return (savedStyle & (WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS)) |
		WS_POPUP;
}

inline DWORD BorderlessWindowExStyle(DWORD savedExStyle)
{
	// These extended styles can reintroduce a non-client edge even when the
	// base style is WS_POPUP.  D3D11 borderless presentation must also remain
	// a normal Alt-Tab participant instead of covering other applications.
	return savedExStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE |
		WS_EX_STATICEDGE | WS_EX_WINDOWEDGE | WS_EX_TOPMOST);
}

inline bool IsBorderlessWindowStyle(DWORD style)
{
	return (style & WS_POPUP) != 0 && (style & WS_CAPTION) == 0 &&
		(style & (WS_THICKFRAME | WS_SYSMENU)) == 0;
}

inline bool ReadWindowStyle(HWND window, int index, DWORD *value)
{
	if (window == 0 || value == 0)
	{
		return false;
	}
	SetLastError(ERROR_SUCCESS);
	const LONG current = GetWindowLong(window, index);
	if (current == 0 && GetLastError() != ERROR_SUCCESS)
	{
		return false;
	}
	*value = static_cast<DWORD>(current);
	return true;
}

inline bool WriteWindowStyle(HWND window, int index, DWORD value)
{
	if (window == 0)
	{
		return false;
	}
	SetLastError(ERROR_SUCCESS);
	const LONG previous = GetWindowLong(window, index);
	if (previous == 0 && GetLastError() != ERROR_SUCCESS)
	{
		return false;
	}
	SetLastError(ERROR_SUCCESS);
	const LONG replaced = SetWindowLong(window, index, static_cast<LONG>(value));
	return replaced != 0 || GetLastError() == ERROR_SUCCESS;
}

inline WindowPresentationPlan ChooseWindowPresentationPlan(
	bool d3d11Backend,
	bool windowed,
	DWORD savedStyle,
	DWORD savedExStyle,
	const RECT &savedWindowRect,
	const RECT &monitorRect)
{
	WindowPresentationPlan plan;
	plan.style = savedStyle;
	plan.exStyle = savedExStyle;
	plan.rect = savedWindowRect;

	if (d3d11Backend && !windowed)
	{
		plan.kind = WINDOW_PRESENTATION_BORDERLESS_FULLSCREEN;
		plan.style = BorderlessWindowStyle(savedStyle);
		plan.exStyle = BorderlessWindowExStyle(savedExStyle);
		plan.rect = monitorRect;
		plan.usesExclusiveMode = false;
	}
	else if (!d3d11Backend && !windowed)
	{
		plan.kind = WINDOW_PRESENTATION_LEGACY_EXCLUSIVE_FULLSCREEN;
		plan.usesExclusiveMode = true;
	}
	else
	{
		plan.kind = WINDOW_PRESENTATION_WINDOWED;
		plan.usesExclusiveMode = false;
	}

	return plan;
}

// The snapshot is retained only while the D3D11 window is borderless.  It
// captures the caller's original captioned style, extended style, and normal
// placement so a toggle back to windowed mode is lossless.
struct WindowPresentationState
{
	WindowPresentationState() : valid(false), style(0), exStyle(0)
	{
		ZeroMemory(&placement, sizeof(placement));
		placement.length = sizeof(placement);
	}

	void clear()
	{
		valid = false;
		style = 0;
		exStyle = 0;
		ZeroMemory(&placement, sizeof(placement));
		placement.length = sizeof(placement);
	}

	bool capture(HWND window)
	{
		if (valid)
		{
			return true;
		}
		if (window == 0 || !IsWindow(window))
		{
			return false;
		}

		WINDOWPLACEMENT currentPlacement;
		ZeroMemory(&currentPlacement, sizeof(currentPlacement));
		currentPlacement.length = sizeof(currentPlacement);
		if (!GetWindowPlacement(window, &currentPlacement))
		{
			return false;
		}

		if (!ReadWindowStyle(window, GWL_STYLE, &style) ||
			!ReadWindowStyle(window, GWL_EXSTYLE, &exStyle))
		{
			return false;
		}
		placement = currentPlacement;
		valid = true;
		return true;
	}

	bool valid;
	DWORD style;
	DWORD exStyle;
	WINDOWPLACEMENT placement;
};

inline bool RestoreWindowPresentationSnapshot(HWND window,
	WindowPresentationState *state, bool clearState)
{
	if (window == 0 || !IsWindow(window) || state == 0 || !state->valid)
	{
		return false;
	}

	bool restored = true;
	if (!WriteWindowStyle(window, GWL_STYLE, state->style))
	{
		restored = false;
	}
	if (!WriteWindowStyle(window, GWL_EXSTYLE, state->exStyle))
	{
		restored = false;
	}
	const HWND restoredOrder = (state->exStyle & WS_EX_TOPMOST) != 0 ?
		HWND_TOPMOST : HWND_NOTOPMOST;
	if (!SetWindowPos(window, restoredOrder, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED))
	{
		restored = false;
	}

	WINDOWPLACEMENT placement = state->placement;
	placement.length = sizeof(placement);
	if (!SetWindowPlacement(window, &placement))
	{
		restored = false;
	}
	if (restored && clearState)
	{
		state->clear();
	}
	return restored;
}

inline bool ApplyBorderlessWindow(HWND window, const RECT &monitorRect,
	WindowPresentationState *state)
{
	if (window == 0 || !IsWindow(window) || state == 0 ||
		!IsValidWindowPresentationRect(monitorRect))
	{
		return false;
	}

	// The persistent state is the windowed restoration point.  A refresh while
	// already fullscreen needs a separate attempt snapshot so a failed monitor
	// move restores the prior borderless window without discarding that point.
	const bool refreshingBorderless = state->valid;
	WindowPresentationState attemptSnapshot;
	WindowPresentationState *failureSnapshot = state;
	if (refreshingBorderless)
	{
		if (!attemptSnapshot.capture(window))
		{
			return false;
		}
		failureSnapshot = &attemptSnapshot;
	}
	else if (!state->capture(window))
	{
		return false;
	}

	const WindowPresentationPlan plan = ChooseWindowPresentationPlan(
		true, false, state->style, state->exStyle,
		state->placement.rcNormalPosition, monitorRect);
	if (!WriteWindowStyle(window, GWL_STYLE, plan.style))
	{
		const DWORD error = GetLastError();
		RestoreWindowPresentationSnapshot(window, failureSnapshot, true);
		SetLastError(error);
		return false;
	}
	if (!WriteWindowStyle(window, GWL_EXSTYLE, plan.exStyle))
	{
		const DWORD error = GetLastError();
		RestoreWindowPresentationSnapshot(window, failureSnapshot, true);
		SetLastError(error);
		return false;
	}
	ShowWindow(window, SW_SHOWNORMAL);
	if (SetWindowPos(window, HWND_NOTOPMOST,
		plan.rect.left, plan.rect.top,
		plan.rect.right - plan.rect.left,
		plan.rect.bottom - plan.rect.top,
		SWP_FRAMECHANGED | SWP_SHOWWINDOW) != FALSE)
	{
		SetLastError(ERROR_SUCCESS);
		DWORD actualStyle = 0;
		DWORD actualExStyle = 0;
		RECT actualWindowRect = { 0 };
		RECT actualClientRect = { 0 };
		if (ReadWindowStyle(window, GWL_STYLE, &actualStyle) &&
			ReadWindowStyle(window, GWL_EXSTYLE, &actualExStyle) &&
			GetWindowRect(window, &actualWindowRect) &&
			GetClientRect(window, &actualClientRect) &&
			IsBorderlessWindowStyle(actualStyle) &&
			(actualExStyle & WS_EX_TOPMOST) == 0 &&
			EqualWindowPresentationRect(actualWindowRect, monitorRect) &&
			actualClientRect.left == 0 && actualClientRect.top == 0 &&
			actualClientRect.right == monitorRect.right - monitorRect.left &&
			actualClientRect.bottom == monitorRect.bottom - monitorRect.top)
		{
			return true;
		}
		if (GetLastError() == ERROR_SUCCESS)
		{
			SetLastError(ERROR_INVALID_DATA);
		}
	}
	const DWORD error = GetLastError();
	RestoreWindowPresentationSnapshot(window, failureSnapshot, true);
	SetLastError(error);
	return false;
}

inline bool RestoreWindowedWindow(HWND window, WindowPresentationState *state)
{
	if (window == 0 || !IsWindow(window) || state == 0 || !state->valid)
	{
		return false;
	}

	return RestoreWindowPresentationSnapshot(window, state, true);
}

} // namespace render
} // namespace rts

#endif
