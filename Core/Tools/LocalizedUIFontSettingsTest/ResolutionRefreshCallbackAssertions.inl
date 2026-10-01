/*
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

void CheckCallbackEvents(const std::vector<std::string>& expected)
{
	CALLBACK_CHECK(callbackEvents == expected);
}

void CheckDisplayTuple(const ResolutionDisplay& display, Int x, Int y, Int depth, Bool windowed)
{
	CALLBACK_CHECK(display.width == x);
	CALLBACK_CHECK(display.height == y);
	CALLBACK_CHECK(display.bitDepth == depth);
	CALLBACK_CHECK(display.windowed == windowed);
}

Int RunResolutionRefreshCallbackTests()
{
	callbackTestFailures = 0;

	// Execute the actual source tail for applying a new resolution and the complete actual decline callback.
	// Starting fullscreen also checks restoration of the windowed/fullscreen tuple and bit depth.
	ResolutionCallbackFixture success;
	success.resolutionCombo.selected = 1;
	ApplyOptionsResolutionTail();
	CheckDisplayTuple(success.display, 1920, 1080, 24, FALSE);
	CALLBACK_CHECK(TheGlobalData->m_xResolution == 1920 && TheGlobalData->m_yResolution == 1080);
	CALLBACK_CHECK(dispChanged == TRUE);
	CALLBACK_CHECK(success.customOverlay.fontSize == 20);
	CALLBACK_CHECK(success.localizedOverlay.fontSize == 22);
	CALLBACK_CHECK(success.customOverlay.text == "Latency: 42 ms");
	CALLBACK_CHECK(success.localizedOverlay.text == "Localized label");
	CheckCallbackEvents(std::vector<std::string>{
		"display-mode", "language-reload", "header-refresh", "mouse-refresh",
		"layout-recreate", "controlbar-recreate", "custom-refresh", "localized-refresh"});
	CALLBACK_CHECK(newDispSettings.xRes == 1920 && newDispSettings.yRes == 1080);
	CALLBACK_CHECK(newDispSettings.bitDepth == 24 && newDispSettings.windowed == FALSE);

	callbackEvents.clear();
	DeclineResolution();
	CheckDisplayTuple(success.display, 1280, 720, 32, FALSE);
	CALLBACK_CHECK(TheGlobalData->m_xResolution == 1280 && TheGlobalData->m_yResolution == 720);
	CALLBACK_CHECK(dispChanged == FALSE);
	CALLBACK_CHECK(oldDispSettings.xRes == 1280 && oldDispSettings.yRes == 720);
	CALLBACK_CHECK(oldDispSettings.bitDepth == 32 && oldDispSettings.windowed == FALSE);
	CALLBACK_CHECK(newDispSettings.xRes == 1280 && newDispSettings.yRes == 720);
	CALLBACK_CHECK(newDispSettings.bitDepth == 32 && newDispSettings.windowed == FALSE);
	CALLBACK_CHECK(success.customOverlay.fontSize == 10);
	CALLBACK_CHECK(success.localizedOverlay.fontSize == 11);
	CALLBACK_CHECK(success.customOverlay.text == "Latency: 42 ms");
	CALLBACK_CHECK(success.localizedOverlay.text == "Localized label");
	CheckCallbackEvents(std::vector<std::string>{
		"display-mode", "language-reload", "header-refresh", "mouse-refresh",
		"preferences-write", "layout-recreate", "controlbar-recreate",
		"custom-refresh", "localized-refresh"});
	CALLBACK_CHECK(OptionPreferences::writeCount == 1);

	// A failed display-mode revert must not claim the old resolution or refresh either font set.
	ResolutionCallbackFixture failedRevert;
	failedRevert.resolutionCombo.selected = 1;
	ApplyOptionsResolutionTail();
	CALLBACK_CHECK(failedRevert.customOverlay.fontSize == 20);
	CALLBACK_CHECK(failedRevert.localizedOverlay.fontSize == 22);
	callbackEvents.clear();
	failedRevert.display.failNextMode = TRUE;
	DeclineResolution();
	CheckDisplayTuple(failedRevert.display, 1920, 1080, 24, FALSE);
	CALLBACK_CHECK(TheGlobalData->m_xResolution == 1920 && TheGlobalData->m_yResolution == 1080);
	CALLBACK_CHECK(dispChanged == TRUE);
	CALLBACK_CHECK(failedRevert.customOverlay.fontSize == 20);
	CALLBACK_CHECK(failedRevert.localizedOverlay.fontSize == 22);
	CALLBACK_CHECK(failedRevert.customOverlay.text == "Latency: 42 ms");
	CALLBACK_CHECK(failedRevert.localizedOverlay.text == "Localized label");
	CheckCallbackEvents(std::vector<std::string>{"display-mode"});
	CALLBACK_CHECK(OptionPreferences::writeCount == 0);

	// Existing code intentionally treats a same-resolution selection as unchanged, even when its
	// selected mode descriptor carries a different bit depth.
	ResolutionCallbackFixture unchanged;
	unchanged.display.windowed = TRUE;
	unchanged.resolutionCombo.selected = 0;
	ApplyOptionsResolutionTail();
	CheckDisplayTuple(unchanged.display, 1280, 720, 32, TRUE);
	CALLBACK_CHECK(!dispChanged);
	CALLBACK_CHECK(unchanged.customOverlay.fontSize == 10);
	CALLBACK_CHECK(unchanged.localizedOverlay.fontSize == 11);
	CheckCallbackEvents(std::vector<std::string>());

	return static_cast<Int>(callbackTestFailures);
}
