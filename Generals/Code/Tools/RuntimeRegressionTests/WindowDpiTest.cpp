#include <windows.h>

#include <stdio.h>

#include "WindowDpi.h"

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE ((HANDLE)-3)
#endif

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

	class ScopedPerMonitorV1ThreadContext
	{
		typedef HANDLE (WINAPI *SetThreadDpiAwarenessContextProc)(HANDLE);

	public:
		ScopedPerMonitorV1ThreadContext() :
			m_setContext(NULL),
			m_previousContext(NULL)
		{
			HMODULE user32 = GetModuleHandleA("user32.dll");
			FARPROC proc = user32 ? GetProcAddress(user32, "SetThreadDpiAwarenessContext") : NULL;
			if (proc)
			{
				m_setContext = reinterpret_cast<SetThreadDpiAwarenessContextProc>(proc);
				m_previousContext = m_setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
			}
		}

		~ScopedPerMonitorV1ThreadContext()
		{
			if (m_setContext && m_previousContext)
				m_setContext(m_previousContext);
		}

		bool IsActive() const
		{
			return m_setContext != NULL && m_previousContext != NULL;
		}

	private:
		SetThreadDpiAwarenessContextProc m_setContext;
		HANDLE m_previousContext;
	};

	bool VerifyPm1AppliedLinearSuggestionFallback()
	{
		ScopedPerMonitorV1ThreadContext dpiContext;
		if (!dpiContext.IsActive())
		{
			puts("PMv1 applied-window DPI check skipped: SetThreadDpiAwarenessContext is unavailable");
			return true;
		}

		// Create the host window as PMv1 without enabling non-client DPI scaling,
		// matching the game window's legacy-awareness fallback.
		HWND window = CreateWindowExA(0, kWindowClassName, "", kWindowStyle,
			20, 20, 640, 480, NULL, NULL, GetModuleHandleA(NULL), NULL);
		if (!window)
			return Fail("could not create a PMv1-aware fallback test window");

		bool success = false;
		do
		{
			const LONG clientWidth = 800;
			const LONG clientHeight = 600;
			RECT initialClient;
			RECT initialWindow;
			if (!GetClientRect(window, &initialClient) || !GetWindowRect(window, &initialWindow))
			{
				Fail("could not inspect the PMv1 fallback test window");
				break;
			}

			const LONG frameWidth = (initialWindow.right - initialWindow.left) -
				(initialClient.right - initialClient.left);
			const LONG frameHeight = (initialWindow.bottom - initialWindow.top) -
				(initialClient.bottom - initialClient.top);
			if (frameWidth < 0 || frameHeight < 0 ||
				!SetWindowPos(window, NULL, initialWindow.left, initialWindow.top,
					clientWidth + frameWidth, clientHeight + frameHeight,
					SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE))
			{
				Fail("could not prepare the PMv1 800x600 client window");
				break;
			}

			RECT actualClient;
			RECT currentWindow;
			if (!GetClientRect(window, &actualClient) || !GetWindowRect(window, &currentWindow) ||
				actualClient.right - actualClient.left != clientWidth ||
				actualClient.bottom - actualClient.top != clientHeight)
			{
				Fail("PMv1 test window did not reach the exact 800x600 client size");
				break;
			}

			RECT suggested;
			suggested.left = currentWindow.left + 29;
			suggested.top = currentWindow.top + 37;
			suggested.right = suggested.left + (currentWindow.right - currentWindow.left) * 3 / 2;
			suggested.bottom = suggested.top + (currentWindow.bottom - currentWindow.top) * 3 / 2;
			const LONG suggestedLeft = suggested.left;
			const LONG suggestedTop = suggested.top;
			if (!WindowDpi::AdjustDpiChangedRectForClientSize(window, &suggested) ||
				!WindowDpi::ApplyDpiChangedRect(window, &suggested))
			{
				Fail("PMv1 fallback could not apply the corrected DPI suggestion");
				break;
			}

			RECT finalClient;
			RECT finalWindow;
			if (!GetClientRect(window, &finalClient) || !GetWindowRect(window, &finalWindow))
			{
				Fail("could not inspect the applied PMv1 fallback window");
				break;
			}

			if (finalClient.right - finalClient.left != clientWidth ||
				finalClient.bottom - finalClient.top != clientHeight)
			{
				Fail("applied 1.5x PMv1 DPI suggestion changed the 800x600 client size");
				break;
			}
			if (finalWindow.left != suggestedLeft || finalWindow.top != suggestedTop)
			{
				Fail("PMv1 fallback did not retain the suggested cursor-relative position");
				break;
			}

			success = true;
		} while (false);

		DestroyWindow(window);
		if (success)
			puts("PMv1 applied-window DPI fallback passed");
		return success;
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
		if (!preservedClientSize)
		{
			DestroyWindow(window);
			return Fail("WM_DPICHANGED altered the client/render dimensions");
		}
		if (!honoredSuggestedPosition)
		{
			DestroyWindow(window);
			return Fail("WM_DPICHANGED did not honor the suggested position");
		}
		if (!VerifyPm1AppliedLinearSuggestionFallback())
		{
			DestroyWindow(window);
			return false;
		}
		DestroyWindow(window);

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
