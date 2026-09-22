#ifndef WINDOW_DPI_H
#define WINDOW_DPI_H

#include <windows.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

#ifndef WM_GETDPISCALEDSIZE
#define WM_GETDPISCALEDSIZE 0x02E4
#endif

// Window-only DPI geometry helpers. These deliberately do not call the renderer:
// a DPI move changes non-client metrics, but the game's client/render resolution
// remains the same.
namespace WindowDpi
{
	typedef BOOL (WINAPI *AdjustWindowRectExForDpiProc)(
		LPRECT, DWORD, BOOL, DWORD, UINT);

	static inline BOOL AdjustWindowRectForDpi(
		LPRECT rect, DWORD style, BOOL hasMenu, DWORD exStyle, UINT dpi)
	{
		HMODULE user32 = ::GetModuleHandleA("user32.dll");
		if (user32)
		{
			FARPROC proc = ::GetProcAddress(user32, "AdjustWindowRectExForDpi");
			if (proc)
			{
				AdjustWindowRectExForDpiProc adjustForDpi =
					reinterpret_cast<AdjustWindowRectExForDpiProc>(proc);
				if (adjustForDpi(rect, style, hasMenu, exStyle, dpi))
					return TRUE;
			}
		}

		// Older Windows releases do not export AdjustWindowRectExForDpi.
		return ::AdjustWindowRectEx(rect, style, hasMenu, exStyle);
	}

	static inline BOOL GetWindowSizeForClientAtDpi(
		HWND window, LONG clientWidth, LONG clientHeight, UINT dpi, SIZE *windowSize)
	{
		if (!window || !windowSize || clientWidth <= 0 || clientHeight <= 0 || dpi == 0)
			return FALSE;

		RECT rect;
		rect.left = 0;
		rect.top = 0;
		rect.right = clientWidth;
		rect.bottom = clientHeight;

		const DWORD style = (DWORD)::GetWindowLongA(window, GWL_STYLE);
		const DWORD exStyle = (DWORD)::GetWindowLongA(window, GWL_EXSTYLE);
		const BOOL hasMenu = ::GetMenu(window) != NULL;
		if (!AdjustWindowRectForDpi(&rect, style, hasMenu, exStyle, dpi))
			return FALSE;

		const LONG width = rect.right - rect.left;
		const LONG height = rect.bottom - rect.top;
		if (width <= 0 || height <= 0)
			return FALSE;

		windowSize->cx = width;
		windowSize->cy = height;
		return TRUE;
	}

	// Fullscreen dimensions belong to the display-mode owner. Explicitly write
	// the unchanged pending size back before returning TRUE from
	// WM_GETDPISCALEDSIZE so Windows does not apply its linear DPI scaling.
	static inline BOOL PreservePendingWindowSize(SIZE *size)
	{
		if (!size || size->cx <= 0 || size->cy <= 0)
			return FALSE;

		const LONG width = size->cx;
		const LONG height = size->cy;
		size->cx = width;
		size->cy = height;
		return TRUE;
	}

	// WM_GETDPISCALEDSIZE gives a pending outer-window size. Preserve its client
	// dimensions (including an intentional renderer-resolution change) while
	// recalculating only the DPI-dependent non-client frame.
	static inline BOOL AdjustPendingWindowSizeForDpi(HWND window, UINT dpi, SIZE *size)
	{
		if (!window || !size)
			return FALSE;

		RECT currentWindow;
		RECT currentClient;
		if (!::GetWindowRect(window, &currentWindow) ||
			!::GetClientRect(window, &currentClient))
			return FALSE;

		const LONG frameWidth = (currentWindow.right - currentWindow.left) -
			(currentClient.right - currentClient.left);
		const LONG frameHeight = (currentWindow.bottom - currentWindow.top) -
			(currentClient.bottom - currentClient.top);
		const LONG clientWidth = size->cx - frameWidth;
		const LONG clientHeight = size->cy - frameHeight;
		if (frameWidth < 0 || frameHeight < 0 || clientWidth <= 0 || clientHeight <= 0)
			return FALSE;

		SIZE adjustedSize;
		if (!GetWindowSizeForClientAtDpi(
			window, clientWidth, clientHeight, dpi, &adjustedSize))
			return FALSE;

		*size = adjustedSize;
		return TRUE;
	}

	// WM_GETDPISCALEDSIZE supplies the exact client-preserving size that Windows
	// places in this suggested rect. Applying it as-is also preserves the
	// cursor-relative position and any pending renderer-resolution change.
	static inline BOOL ApplyDpiChangedRect(HWND window, const RECT *suggestedRect)
	{
		if (!window || !suggestedRect)
			return FALSE;

		const LONG width = suggestedRect->right - suggestedRect->left;
		const LONG height = suggestedRect->bottom - suggestedRect->top;
		if (width <= 0 || height <= 0)
			return FALSE;

		return ::SetWindowPos(window, NULL,
			suggestedRect->left,
			suggestedRect->top,
			width,
			height,
			SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
	}

	// Per-monitor-v1 windows do not receive WM_GETDPISCALEDSIZE. Their
	// WM_DPICHANGED suggestion is linearly scaled, so preserve the current client
	// dimensions and use the suggestion only for cursor-relative positioning.
	static inline BOOL AdjustDpiChangedRectForClientSize(
		HWND window, RECT *suggestedRect, UINT dpi)
	{
		if (!window || !suggestedRect)
			return FALSE;

		RECT clientRect;
		if (!::GetClientRect(window, &clientRect))
			return FALSE;

		const LONG clientWidth = clientRect.right - clientRect.left;
		const LONG clientHeight = clientRect.bottom - clientRect.top;
		SIZE windowSize;
		if (!GetWindowSizeForClientAtDpi(
			window, clientWidth, clientHeight, dpi, &windowSize))
			return FALSE;

		const LONG left = suggestedRect->left;
		const LONG top = suggestedRect->top;
		suggestedRect->right = left + windowSize.cx;
		suggestedRect->bottom = top + windowSize.cy;
		return TRUE;
	}
}

#endif // WINDOW_DPI_H
