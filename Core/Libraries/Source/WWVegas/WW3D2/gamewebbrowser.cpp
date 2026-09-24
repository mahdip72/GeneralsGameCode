/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
**
** Native renderer browser facade.  The embedded browser is disabled on the
** native product path, so these operations intentionally retain the legacy
** disabled-feature behavior without creating a renderer dependency.
*/

#include "Renderer/GameWebBrowser.h"

namespace rts
{
namespace render
{

bool GameWebBrowser::Initialize(const char *, const char *, const char *, const char *)
{
	return false;
}

void GameWebBrowser::Shutdown()
{
}

void GameWebBrowser::Update()
{
}

void GameWebBrowser::Render(int)
{
}

void GameWebBrowser::CreateBrowser(const char *, const char *, int, int, int, int,
	int, long, void *)
{
}

void GameWebBrowser::DestroyBrowser(const char *)
{
}

bool GameWebBrowser::IsBrowserOpen(const char *)
{
	return false;
}

void GameWebBrowser::Navigate(const char *, const char *)
{
}

} // namespace render
} // namespace rts
