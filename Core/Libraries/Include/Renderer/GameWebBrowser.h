/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
**
** Renderer-neutral browser lifecycle and window operations.
*/

#pragma once

namespace rts
{
namespace render
{

enum GameWebBrowserOption
{
	GAME_WEB_BROWSER_OPTION_SCROLLBARS = 0x0001,
	GAME_WEB_BROWSER_OPTION_3D_BORDER = 0x0002
};

// The title layer uses this facade so browser ownership stays outside the
// native renderer contract.  The x64 implementation is a truthful disabled
// feature stub; the x86 compatibility target supplies the historical adapter.
class GameWebBrowser
{
public:
	static bool Initialize(const char *badPageUrl = 0,
		const char *loadingPageUrl = 0,
		const char *mouseFileName = 0,
		const char *mouseBusyFileName = 0);

	static void Shutdown();
	static void Update();
	static void Render(int backBufferIndex);

	static void CreateBrowser(const char *browserName,
		const char *url,
		int x,
		int y,
		int width,
		int height,
		int updateTicks = 0,
		long options = GAME_WEB_BROWSER_OPTION_SCROLLBARS |
			GAME_WEB_BROWSER_OPTION_3D_BORDER,
		void *gameDispatch = 0);

	static void DestroyBrowser(const char *browserName);
	static bool IsBrowserOpen(const char *browserName);
	static void Navigate(const char *browserName, const char *url);
};

} // namespace render
} // namespace rts
