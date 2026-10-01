/*
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

typedef int Int;
typedef unsigned int UnsignedInt;
typedef bool Bool;
enum { FALSE = false, TRUE = true };

static unsigned int callbackTestFailures = 0;
static std::vector<std::string> callbackEvents;

void TraceCallbackEvent(const char* event)
{
	callbackEvents.push_back(event);
}

void CheckCallback(bool condition, const char* expression, Int line)
{
	if (!condition)
	{
		++callbackTestFailures;
		std::fprintf(stderr, "line %d: %s\n", line, expression);
	}
}

#define CALLBACK_CHECK(condition) CheckCallback((condition), #condition, static_cast<Int>(__LINE__))

struct AsciiString
{
	std::string value;
	AsciiString() {}
	AsciiString(const char* initial) : value(initial ? initial : "") {}
	AsciiString(const AsciiString&) = default;
	AsciiString& operator=(const AsciiString&) = default;
	AsciiString& operator=(const char* initial) { value = initial ? initial : ""; return *this; }
	void format(const char* formatString, Int first, Int second)
	{
		char buffer[64];
		std::snprintf(buffer, sizeof(buffer), formatString, first, second);
		value = buffer;
	}
};

struct DisplaySettings
{
	Int xRes;
	Int yRes;
	Int bitDepth;
	Bool windowed;
	DisplaySettings() : xRes(0), yRes(0), bitDepth(0), windowed(true) {}
};

struct ResolutionMode
{
	Int xRes;
	Int yRes;
	Int bitDepth;
	ResolutionMode(Int x, Int y, Int depth) : xRes(x), yRes(y), bitDepth(depth) {}
};

class ResolutionDisplay;
extern ResolutionDisplay* TheDisplay;

struct GlobalLanguageData
{
	Int fontScale;
	GlobalLanguageData() : fontScale(1) {}
	void onResolutionChanged();
	Int adjustFontSize(Int baseSize) const { return baseSize * fontScale; }
};
extern GlobalLanguageData* TheGlobalLanguageData;

struct ResolutionDisplay
{
	Int width;
	Int height;
	Int bitDepth;
	Bool windowed;
	Bool failNextMode;
	std::vector<ResolutionMode> modes;
	ResolutionDisplay() : width(1280), height(720), bitDepth(32), windowed(false), failNextMode(false) {}
	Bool setDisplayMode(Int x, Int y, Int depth, Bool isWindowed)
	{
		TraceCallbackEvent("display-mode");
		if (failNextMode)
		{
			failNextMode = false;
			return FALSE;
		}
		width = x;
		height = y;
		bitDepth = depth;
		windowed = isWindowed;
		return TRUE;
	}
	Int getWidth() const { return width; }
	Int getHeight() const { return height; }
	Int getBitDepth() const { return bitDepth; }
	Bool getWindowed() const { return windowed; }
	Int getDisplayModeCount() const { return static_cast<Int>(modes.size()); }
	void getDisplayModeDescription(Int index, Int* x, Int* y, Int* depth) const
	{
		*x = modes[index].xRes;
		*y = modes[index].yRes;
		*depth = modes[index].bitDepth;
	}
};

void GlobalLanguageData::onResolutionChanged()
{
	TraceCallbackEvent("language-reload");
	fontScale = TheDisplay->getWidth() >= 1600 ? 2 : 1;
}

struct GlobalData
{
	Int m_xResolution;
	Int m_yResolution;
	GlobalData() : m_xResolution(1280), m_yResolution(720) {}
};

struct HeaderTemplateManager
{
	void onResolutionChanged() { TraceCallbackEvent("header-refresh"); }
};

struct Mouse
{
	void onResolutionChanged() { TraceCallbackEvent("mouse-refresh"); }
};

struct DisplayString
{
	std::string text;
	Int fontSize;
	DisplayString(const char* initialText, Int initialSize) : text(initialText), fontSize(initialSize) {}
};

struct InGameUI
{
	DisplayString* customOverlay;
	DisplayString* localizedOverlay;
	InGameUI() : customOverlay(nullptr), localizedOverlay(nullptr) {}
	void recreateControlBar() { TraceCallbackEvent("controlbar-recreate"); }
	void refreshCustomUiResources()
	{
		TraceCallbackEvent("custom-refresh");
		if (customOverlay && TheGlobalLanguageData)
			customOverlay->fontSize = TheGlobalLanguageData->adjustFontSize(10);
	}
	void refreshLocalizedFontResources()
	{
		TraceCallbackEvent("localized-refresh");
		if (localizedOverlay && TheGlobalLanguageData)
			localizedOverlay->fontSize = TheGlobalLanguageData->adjustFontSize(11);
	}
};

struct Shell
{
	void recreateWindowLayouts() { TraceCallbackEvent("layout-recreate"); }
};

struct ResolutionComboBox
{
	Int selected;
	Bool enabled;
	ResolutionComboBox() : selected(0), enabled(true) {}
	Bool winGetEnabled() const { return enabled; }
};

void GadgetComboBoxGetSelectedPos(ResolutionComboBox* combo, Int* selected)
{
	*selected = combo ? combo->selected : -1;
}

struct OptionPreferences
{
	std::map<std::string, AsciiString> values;
	inline static Int writeCount = 0;
	AsciiString& operator[](const char* key) { return values[key]; }
	void write() { ++writeCount; TraceCallbackEvent("preferences-write"); }
};

ResolutionDisplay* TheDisplay = nullptr;
GlobalData* TheGlobalData = nullptr;
GlobalData* TheWritableGlobalData = nullptr;
GlobalLanguageData* TheGlobalLanguageData = nullptr;
HeaderTemplateManager* TheHeaderTemplateManager = nullptr;
Mouse* TheMouse = nullptr;
Shell* TheShell = nullptr;
InGameUI* TheInGameUI = nullptr;
ResolutionComboBox* comboBoxResolution = nullptr;
OptionPreferences* pref = nullptr;
DisplaySettings oldDispSettings;
DisplaySettings newDispSettings;
Bool dispChanged = FALSE;

struct ResolutionCallbackFixture
{
	ResolutionDisplay display;
	GlobalData globalData;
	GlobalLanguageData language;
	HeaderTemplateManager header;
	Mouse mouse;
	Shell shell;
	InGameUI inGameUI;
	ResolutionComboBox resolutionCombo;
	OptionPreferences preferences;
	DisplayString customOverlay;
	DisplayString localizedOverlay;
	ResolutionCallbackFixture()
		: customOverlay("Latency: 42 ms", 10), localizedOverlay("Localized label", 11)
	{
		display.modes.push_back(ResolutionMode(1280, 720, 16));
		display.modes.push_back(ResolutionMode(1920, 1080, 24));
		inGameUI.customOverlay = &customOverlay;
		inGameUI.localizedOverlay = &localizedOverlay;
		install();
	}
	void install()
	{
		TheDisplay = &display;
		TheGlobalData = &globalData;
		TheWritableGlobalData = &globalData;
		TheGlobalLanguageData = &language;
		TheHeaderTemplateManager = &header;
		TheMouse = &mouse;
		TheShell = &shell;
		TheInGameUI = &inGameUI;
		comboBoxResolution = &resolutionCombo;
		pref = &preferences;
		oldDispSettings = DisplaySettings();
		newDispSettings = DisplaySettings();
		dispChanged = FALSE;
		OptionPreferences::writeCount = 0;
		callbackEvents.clear();
	}
};
