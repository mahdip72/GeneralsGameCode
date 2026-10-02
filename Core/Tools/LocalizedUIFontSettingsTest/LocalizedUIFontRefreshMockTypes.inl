/*
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

typedef int Int;
typedef unsigned int UnsignedInt;
typedef bool Bool;
enum { FALSE = false, TRUE = true };
enum { MAX_UI_MESSAGES = 3, MAX_PLAYER_COUNT = 2, MAX_SUBTITLE_LINES = 4 };

struct AsciiString
{
	std::string value;
	AsciiString() {}
	AsciiString(const char* text) : value(text ? text : "") {}
	AsciiString(const std::string& text) : value(text) {}
	AsciiString(const AsciiString&) = default;
	AsciiString& operator=(const AsciiString&) = default;
	AsciiString& operator=(const char* text) { value = text ? text : ""; return *this; }
	AsciiString& operator=(const std::string& text) { value = text; return *this; }
	Bool isNotEmpty() const { return !value.empty(); }
	Bool operator<(const AsciiString& other) const { return value < other.value; }
};

struct FontDesc
{
	AsciiString name;
	Int size;
	Bool bold;
};

struct GlobalLanguageData
{
	FontDesc m_drawableCaptionFont;
	FontDesc m_messageFont;
	FontDesc m_militaryCaptionTitleFont;
	FontDesc m_militaryCaptionFont;
	FontDesc m_superweaponCountdownNormalFont;
	FontDesc m_superweaponCountdownReadyFont;
	FontDesc m_namedTimerCountdownNormalFont;
	FontDesc m_namedTimerCountdownReadyFont;
	std::vector<Int> adjustedInputs;
	Int adjustFontSize(Int pointSize) { adjustedInputs.push_back(pointSize); return pointSize + 3; }
};

struct GameFont
{
	std::string name;
	Int pointSize;
	Bool bold;
	GameFont() : pointSize(0), bold(false) {}
	GameFont(const std::string& fontName, Int size, Bool isBold)
		: name(fontName), pointSize(size), bold(isBold) {}
};

struct FontLibrary
{
	std::map<std::string, GameFont> fonts;
	Int lookupCount;
	FontLibrary() : lookupCount(0) {}
	GameFont* getFont(const AsciiString& name, Int pointSize, Bool bold)
	{
		++lookupCount;
		std::ostringstream key;
		key << name.value << '\x1f' << pointSize << '\x1f' << (bold ? 1 : 0);
		const std::string mapKey = key.str();
		std::map<std::string, GameFont>::iterator it = fonts.find(mapKey);
		if (it == fonts.end())
			it = fonts.insert(std::make_pair(mapKey, GameFont(name.value, pointSize, bold))).first;
		return &it->second;
	}
};

struct DisplayString
{
	std::string text;
	GameFont* font;
	Int fontSetCount;
	DisplayString() : font(nullptr), fontSetCount(0) {}
	DisplayString(const char* initialText) : text(initialText), font(nullptr), fontSetCount(0) {}
	void setFont(GameFont* newFont) { font = newFont; ++fontSetCount; }
	void setText(const std::string& newText) { text = newText; }
	const std::string& getText() const { return text; }
	void getSize(Int* width, Int* height) const
	{
		if (width) *width = font ? font->pointSize * static_cast<Int>(text.size()) : 0;
		if (height) *height = font ? font->pointSize + 2 : 0;
	}
};

struct UIMessage { DisplayString* displayString; UIMessage() : displayString(nullptr) {} };

struct SuperweaponInfo
{
	DisplayString* m_nameDisplayString;
	DisplayString* m_timeDisplayString;
	Bool m_ready;
	UnsignedInt m_timestamp;
	void setFont(const AsciiString& name, Int pointSize, Bool bold);
};

struct NamedTimerInfo
{
	AsciiString timerText;
	DisplayString* displayString;
	UnsignedInt timestamp;
	Bool isCountdown;
	NamedTimerInfo() : displayString(nullptr), timestamp(0), isCountdown(false) {}
};

typedef std::map<Int, std::list<SuperweaponInfo*> > SuperweaponMap;
typedef SuperweaponMap::iterator SuperweaponMapIt;
typedef std::list<SuperweaponInfo*> SuperweaponList;
typedef SuperweaponList::iterator SuperweaponListIt;
typedef std::map<AsciiString, NamedTimerInfo*> NamedTimerMap;
typedef NamedTimerMap::iterator NamedTimerMapIt;

struct Point2
{
	Int x;
	Int y;
	Point2() : x(0), y(0) {}
	Point2(Int initialX, Int initialY) : x(initialX), y(initialY) {}
};

struct MilitarySubtitle
{
	Point2 position;
	Point2 blockPos;
	UnsignedInt currentDisplayString;
	DisplayString* displayStrings[MAX_SUBTITLE_LINES];
	MilitarySubtitle() : currentDisplayString(0)
	{
		for (Int i = 0; i < MAX_SUBTITLE_LINES; ++i)
			displayStrings[i] = nullptr;
	}
};

class InGameUI;
class Drawable;
class GameClient;
extern InGameUI* TheInGameUI;
extern GlobalLanguageData* TheGlobalLanguageData;
extern FontLibrary* TheFontLibrary;
extern GameClient* TheGameClient;

class Drawable
{
public:
	Drawable* next;
	Int nextReadCount;
	DisplayString* m_constructDisplayString;
	DisplayString* m_captionDisplayString;
	Drawable() : next(nullptr), nextReadCount(0), m_constructDisplayString(nullptr),
		m_captionDisplayString(nullptr) {}
	Drawable* getNextDrawable() { ++nextReadCount; return next; }
	void refreshCaptionFont();
};

class GameClient
{
public:
	Drawable* head;
	Int firstReadCount;
	GameClient() : head(nullptr), firstReadCount(0) {}
	Drawable* firstDrawable() { ++firstReadCount; return head; }
};

class InGameUI
{
public:
	typedef LocalizedUIFontSettings<AsciiString, Int, Bool> InGameUIFontSettings;
	enum
	{
		UI_FONT_DRAWABLE_CAPTION,
		UI_FONT_MESSAGE,
		UI_FONT_MILITARY_TITLE,
		UI_FONT_MILITARY_BODY,
		UI_FONT_SUPERWEAPON_NORMAL,
		UI_FONT_SUPERWEAPON_READY,
		UI_FONT_NAMED_TIMER_NORMAL,
		UI_FONT_NAMED_TIMER_READY,
		UI_FONT_SETTING_COUNT
	};

	AsciiString m_drawableCaptionFont;
	Int m_drawableCaptionPointSize;
	Bool m_drawableCaptionBold;
	AsciiString m_messageFont;
	Int m_messagePointSize;
	Bool m_messageBold;
	AsciiString m_militaryCaptionTitleFont;
	Int m_militaryCaptionTitlePointSize;
	Bool m_militaryCaptionTitleBold;
	AsciiString m_militaryCaptionFont;
	Int m_militaryCaptionPointSize;
	Bool m_militaryCaptionBold;
	AsciiString m_superweaponNormalFont;
	Int m_superweaponNormalPointSize;
	Bool m_superweaponNormalBold;
	AsciiString m_superweaponReadyFont;
	Int m_superweaponReadyPointSize;
	Bool m_superweaponReadyBold;
	AsciiString m_namedTimerNormalFont;
	Int m_namedTimerNormalPointSize;
	Bool m_namedTimerNormalBold;
	AsciiString m_namedTimerReadyFont;
	Int m_namedTimerReadyPointSize;
	Bool m_namedTimerReadyBold;
	InGameUIFontSettings m_iniFontSettings[UI_FONT_SETTING_COUNT];
	Bool m_iniFontSettingsValid;
	UIMessage m_uiMessages[MAX_UI_MESSAGES];
	SuperweaponMap m_superweapons[MAX_PLAYER_COUNT];
	NamedTimerMap m_namedTimers;
	MilitarySubtitle* m_militarySubtitle;

	InGameUI() : m_drawableCaptionPointSize(0), m_drawableCaptionBold(false),
		m_messagePointSize(0), m_messageBold(false), m_militaryCaptionTitlePointSize(0),
		m_militaryCaptionTitleBold(false), m_militaryCaptionPointSize(0),
		m_militaryCaptionBold(false), m_superweaponNormalPointSize(0),
		m_superweaponNormalBold(false), m_superweaponReadyPointSize(0),
		m_superweaponReadyBold(false), m_namedTimerNormalPointSize(0),
		m_namedTimerNormalBold(false), m_namedTimerReadyPointSize(0),
		m_namedTimerReadyBold(false), m_iniFontSettingsValid(false),
		m_militarySubtitle(nullptr) {}

	void captureIniFontSettings();
	void applyLocalizedFontSetting(Int index, const FontDesc* localized,
		AsciiString& fontName, Int& pointSize, Bool& bold);
	void applyLocalizedFontSettings();
	void refreshLocalizedFontResources();
#ifdef LOCALIZED_UI_FONT_STALE_BASELINE_METHOD
	void applyOriginalLocalizedFontOverride();
#endif
	AsciiString getDrawableCaptionFontName() const { return m_drawableCaptionFont; }
	Int getDrawableCaptionPointSize() const { return m_drawableCaptionPointSize; }
	Bool isDrawableCaptionBold() const { return m_drawableCaptionBold; }
};

InGameUI* TheInGameUI = nullptr;
GlobalLanguageData* TheGlobalLanguageData = nullptr;
FontLibrary* TheFontLibrary = nullptr;
GameClient* TheGameClient = nullptr;
