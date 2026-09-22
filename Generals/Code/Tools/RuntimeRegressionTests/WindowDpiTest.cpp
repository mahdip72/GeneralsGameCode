#include <windows.h>

#include <stdio.h>

#include "WindowDpi.h"

namespace
{
	const char *kWindowClassName = "RTS_WindowDpiContractTest";
	const DWORD kWindowStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

	bool Fail(const char *message)
	{
		fprintf(stderr, "window DPI contract test failed: %s\n", message);
		return false;
	}

	UINT GetWindowDpi(HWND window)
	{
		HMODULE user32 = GetModuleHandleA("user32.dll");
		if (user32)
		{
			typedef UINT (WINAPI *GetDpiForWindowProc)(HWND);
			FARPROC proc = GetProcAddress(user32, "GetDpiForWindow");
			if (proc)
				return reinterpret_cast<GetDpiForWindowProc>(proc)(window);
		}

		return 96;
	}

	bool ExpectWindowSizeForClientAtDpi(HWND window, LONG width, LONG height, UINT dpi)
	{
		SIZE actual;
		if (!WindowDpi::GetWindowSizeForClientAtDpi(window, width, height, dpi, &actual))
			return Fail("production DPI geometry helper rejected valid client dimensions");

		RECT expectedRect = { 0, 0, width, height };
		HMODULE user32 = GetModuleHandleA("user32.dll");
		FARPROC proc = user32 ? GetProcAddress(user32, "AdjustWindowRectExForDpi") : NULL;
		if (proc)
		{
			WindowDpi::AdjustWindowRectExForDpiProc adjustForDpi =
				reinterpret_cast<WindowDpi::AdjustWindowRectExForDpiProc>(proc);
			if (!adjustForDpi(&expectedRect, kWindowStyle, FALSE, 0, dpi))
				return Fail("Windows rejected independent target-DPI frame calculation");
		}
		else if (!AdjustWindowRectEx(&expectedRect, kWindowStyle, FALSE, 0))
		{
			return Fail("Windows rejected independent fallback frame calculation");
		}

		if (actual.cx != expectedRect.right - expectedRect.left ||
			actual.cy != expectedRect.bottom - expectedRect.top)
			return Fail("calculated outer size does not preserve the requested client size");

		return true;
	}

	bool RunWindowDpiContractTest()
	{
		SIZE fullscreenSize = { 1920, 1080 };
		if (!WindowDpi::PreservePendingWindowSize(&fullscreenSize) ||
			fullscreenSize.cx != 1920 || fullscreenSize.cy != 1080)
			return Fail("fullscreen pending dimensions were not explicitly preserved");

		WNDCLASSA windowClass;
		ZeroMemory(&windowClass, sizeof(windowClass));
		windowClass.lpfnWndProc = DefWindowProcA;
		windowClass.hInstance = GetModuleHandleA(NULL);
		windowClass.lpszClassName = kWindowClassName;
		if (!RegisterClassA(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
			return Fail("could not register test window class");

		HWND window = CreateWindowExA(0, kWindowClassName, "", kWindowStyle,
			20, 20, 640, 480, NULL, NULL, windowClass.hInstance, NULL);
		if (!window)
			return Fail("could not create hidden test window");

		RECT initialClient;
		RECT initialWindow;
		if (!GetClientRect(window, &initialClient) || !GetWindowRect(window, &initialWindow))
		{
			DestroyWindow(window);
			return Fail("could not inspect initial test window geometry");
		}

		const LONG initialClientWidth = initialClient.right - initialClient.left;
		const LONG initialClientHeight = initialClient.bottom - initialClient.top;
		const UINT currentDpi = GetWindowDpi(window);
		if (!ExpectWindowSizeForClientAtDpi(window, initialClientWidth + 72,
			initialClientHeight + 48, 144) ||
			!ExpectWindowSizeForClientAtDpi(window, initialClientWidth,
			initialClientHeight, currentDpi))
		{
			DestroyWindow(window);
			return false;
		}

		// Simulate the PMv2 pre-change sizing message with a pending client-size
		// change and verify the helper alters only the target-DPI frame.
		SIZE pending;
		if (!WindowDpi::GetWindowSizeForClientAtDpi(window,
			initialClientWidth + 72,
			initialClientHeight + 48,
			currentDpi,
			&pending) ||
			!WindowDpi::AdjustPendingWindowSizeForDpi(window, 144, &pending))
		{
			DestroyWindow(window);
			return Fail("could not adjust WM_GETDPISCALEDSIZE pending geometry");
		}

		SIZE expectedPending;
		if (!WindowDpi::GetWindowSizeForClientAtDpi(window,
			initialClientWidth + 72,
			initialClientHeight + 48,
			144,
			&expectedPending) ||
			pending.cx != expectedPending.cx || pending.cy != expectedPending.cy)
		{
			DestroyWindow(window);
			return Fail("pending renderer client size changed during DPI frame adjustment");
		}

		// Apply the custom suggested size produced above. Its client dimensions
		// remain exact while its position is retained for the move.
		SIZE sameDpiSize;
		if (!WindowDpi::GetWindowSizeForClientAtDpi(window,
			initialClientWidth, initialClientHeight, currentDpi, &sameDpiSize))
		{
			DestroyWindow(window);
			return Fail("could not calculate the same-DPI suggested window size");
		}
		SIZE sameDpiPending = {
			initialWindow.right - initialWindow.left,
			initialWindow.bottom - initialWindow.top
		};
		if (!WindowDpi::AdjustPendingWindowSizeForDpi(window, currentDpi, &sameDpiPending) ||
			sameDpiPending.cx != sameDpiSize.cx || sameDpiPending.cy != sameDpiSize.cy)
		{
			DestroyWindow(window);
			return Fail("WM_GETDPISCALEDSIZE did not produce the client-preserving suggestion");
		}

		RECT suggested;
		suggested.left = initialWindow.left + 23;
		suggested.top = initialWindow.top + 31;
		suggested.right = suggested.left + sameDpiPending.cx;
		suggested.bottom = suggested.top + sameDpiPending.cy;
		if (!WindowDpi::ApplyDpiChangedRect(window, &suggested))
		{
			DestroyWindow(window);
			return Fail("could not apply WM_DPICHANGED position and frame");
		}

		RECT finalClient;
		RECT finalWindow;
		if (!GetClientRect(window, &finalClient) || !GetWindowRect(window, &finalWindow))
		{
			DestroyWindow(window);
			return Fail("could not inspect post-DPI test window geometry");
		}

		const bool preservedClientSize =
			(finalClient.right - finalClient.left) == initialClientWidth &&
			(finalClient.bottom - finalClient.top) == initialClientHeight;
		const bool honoredSuggestedPosition =
			finalWindow.left == suggested.left && finalWindow.top == suggested.top;
		DestroyWindow(window);
		if (!preservedClientSize)
			return Fail("WM_DPICHANGED altered the client/render dimensions");
		if (!honoredSuggestedPosition)
			return Fail("WM_DPICHANGED did not honor the suggested position");

		return true;
	}
}

int main()
{
	if (!RunWindowDpiContractTest())
		return 1;

	puts("window DPI contract test passed");
	return 0;
}
