/*
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

static unsigned int testFailures = 0;

void Check(bool condition, const char* expression, Int line)
{
	if (!condition)
	{
		++testFailures;
		std::fprintf(stderr, "line %d: %s\n", line, expression);
	}
}

void SetDescriptor(FontDesc& desc, const std::string& name, Int size, Bool bold)
{
	desc.name = name;
	desc.size = size;
	desc.bold = bold;
}

void SetLanguageDescriptors(GlobalLanguageData& language, const char* suffix, Int base)
{
	const std::string tag(suffix);
	SetDescriptor(language.m_drawableCaptionFont, "Caption-" + tag, base, base % 2 != 0);
	SetDescriptor(language.m_messageFont, "Message-" + tag, base + 1, base % 2 == 0);
	SetDescriptor(language.m_militaryCaptionTitleFont, "Title-" + tag, base + 2, base % 2 != 0);
	SetDescriptor(language.m_militaryCaptionFont, "Body-" + tag, base + 3, base % 2 == 0);
	SetDescriptor(language.m_superweaponCountdownNormalFont, "Super-" + tag, base + 4, base % 2 != 0);
	SetDescriptor(language.m_superweaponCountdownReadyFont, "SuperReady-" + tag, base + 5, base % 2 == 0);
	SetDescriptor(language.m_namedTimerCountdownNormalFont, "Timer-" + tag, base + 6, base % 2 != 0);
	SetDescriptor(language.m_namedTimerCountdownReadyFont, "TimerReady-" + tag, base + 7, base % 2 == 0);
}

void ClearLanguageDescriptors(GlobalLanguageData& language)
{
	SetDescriptor(language.m_drawableCaptionFont, "", 0, false);
	SetDescriptor(language.m_messageFont, "", 0, false);
	SetDescriptor(language.m_militaryCaptionTitleFont, "", 0, false);
	SetDescriptor(language.m_militaryCaptionFont, "", 0, false);
	SetDescriptor(language.m_superweaponCountdownNormalFont, "", 0, false);
	SetDescriptor(language.m_superweaponCountdownReadyFont, "", 0, false);
	SetDescriptor(language.m_namedTimerCountdownNormalFont, "", 0, false);
	SetDescriptor(language.m_namedTimerCountdownReadyFont, "", 0, false);
}

void ConfigureIniFallback(InGameUI& ui)
{
	ui.m_drawableCaptionFont = "Caption-INI";
	ui.m_drawableCaptionPointSize = 9;
	ui.m_drawableCaptionBold = false;
	ui.m_messageFont = "Message-INI";
	ui.m_messagePointSize = 10;
	ui.m_messageBold = true;
	ui.m_militaryCaptionTitleFont = "Title-INI";
	ui.m_militaryCaptionTitlePointSize = 12;
	ui.m_militaryCaptionTitleBold = false;
	ui.m_militaryCaptionFont = "Body-INI";
	ui.m_militaryCaptionPointSize = 13;
	ui.m_militaryCaptionBold = true;
	ui.m_superweaponNormalFont = "Super-INI";
	ui.m_superweaponNormalPointSize = 14;
	ui.m_superweaponNormalBold = false;
	ui.m_superweaponReadyFont = "SuperReady-INI";
	ui.m_superweaponReadyPointSize = 15;
	ui.m_superweaponReadyBold = true;
	ui.m_namedTimerNormalFont = "Timer-INI";
	ui.m_namedTimerNormalPointSize = 16;
	ui.m_namedTimerNormalBold = false;
	ui.m_namedTimerReadyFont = "TimerReady-INI";
	ui.m_namedTimerReadyPointSize = 17;
	ui.m_namedTimerReadyBold = true;
}

void Bind(DisplayString& display, FontLibrary& fonts, GlobalLanguageData& language,
	const AsciiString& name, Int pointSize, Bool bold)
{
	display.setFont(fonts.getFont(name, language.adjustFontSize(pointSize), bold));
}

void BindDrawable(Drawable& drawable, FontLibrary& fonts, GlobalLanguageData& language,
	InGameUI& ui)
{
	if (drawable.m_constructDisplayString)
		Bind(*drawable.m_constructDisplayString, fonts, language, ui.m_drawableCaptionFont,
			ui.m_drawableCaptionPointSize, ui.m_drawableCaptionBold);
	if (drawable.m_captionDisplayString)
		Bind(*drawable.m_captionDisplayString, fonts, language, ui.m_drawableCaptionFont,
			ui.m_drawableCaptionPointSize, ui.m_drawableCaptionBold);
}

void CheckBound(const DisplayString* display, const char* name, Int pointSize, Bool bold)
{
	CHECK(display != nullptr);
	if (!display || !display->font)
	{
		CHECK(false);
		return;
	}
	CHECK(display->font->name == name);
	CHECK(display->font->pointSize == pointSize + 3);
	CHECK(display->font->bold == bold);
}

void CheckEffectiveSettings(const InGameUI& ui, const char* suffix, Int base)
{
	const std::string tag(suffix);
	CHECK(ui.m_drawableCaptionFont.value == "Caption-" + tag);
	CHECK(ui.m_drawableCaptionPointSize == base);
	CHECK(ui.m_drawableCaptionBold == (base % 2 != 0));
	CHECK(ui.m_messageFont.value == "Message-" + tag);
	CHECK(ui.m_messagePointSize == base + 1);
	CHECK(ui.m_messageBold == (base % 2 == 0));
	CHECK(ui.m_militaryCaptionTitleFont.value == "Title-" + tag);
	CHECK(ui.m_militaryCaptionTitlePointSize == base + 2);
	CHECK(ui.m_militaryCaptionTitleBold == (base % 2 != 0));
	CHECK(ui.m_militaryCaptionFont.value == "Body-" + tag);
	CHECK(ui.m_militaryCaptionPointSize == base + 3);
	CHECK(ui.m_militaryCaptionBold == (base % 2 == 0));
	CHECK(ui.m_superweaponNormalFont.value == "Super-" + tag);
	CHECK(ui.m_superweaponNormalPointSize == base + 4);
	CHECK(ui.m_superweaponNormalBold == (base % 2 != 0));
	CHECK(ui.m_superweaponReadyFont.value == "SuperReady-" + tag);
	CHECK(ui.m_superweaponReadyPointSize == base + 5);
	CHECK(ui.m_superweaponReadyBold == (base % 2 == 0));
	CHECK(ui.m_namedTimerNormalFont.value == "Timer-" + tag);
	CHECK(ui.m_namedTimerNormalPointSize == base + 6);
	CHECK(ui.m_namedTimerNormalBold == (base % 2 != 0));
	CHECK(ui.m_namedTimerReadyFont.value == "TimerReady-" + tag);
	CHECK(ui.m_namedTimerReadyPointSize == base + 7);
	CHECK(ui.m_namedTimerReadyBold == (base % 2 == 0));
}

void CheckIniSettings(const InGameUI& ui)
{
	CHECK(ui.m_drawableCaptionFont.value == "Caption-INI");
	CHECK(ui.m_drawableCaptionPointSize == 9);
	CHECK(ui.m_drawableCaptionBold == false);
	CHECK(ui.m_messageFont.value == "Message-INI");
	CHECK(ui.m_messagePointSize == 10);
	CHECK(ui.m_messageBold == true);
	CHECK(ui.m_militaryCaptionTitleFont.value == "Title-INI");
	CHECK(ui.m_militaryCaptionTitlePointSize == 12);
	CHECK(ui.m_militaryCaptionTitleBold == false);
	CHECK(ui.m_militaryCaptionFont.value == "Body-INI");
	CHECK(ui.m_militaryCaptionPointSize == 13);
	CHECK(ui.m_militaryCaptionBold == true);
	CHECK(ui.m_superweaponNormalFont.value == "Super-INI");
	CHECK(ui.m_superweaponNormalPointSize == 14);
	CHECK(ui.m_superweaponNormalBold == false);
	CHECK(ui.m_superweaponReadyFont.value == "SuperReady-INI");
	CHECK(ui.m_superweaponReadyPointSize == 15);
	CHECK(ui.m_superweaponReadyBold == true);
	CHECK(ui.m_namedTimerNormalFont.value == "Timer-INI");
	CHECK(ui.m_namedTimerNormalPointSize == 16);
	CHECK(ui.m_namedTimerNormalBold == false);
	CHECK(ui.m_namedTimerReadyFont.value == "TimerReady-INI");
	CHECK(ui.m_namedTimerReadyPointSize == 17);
	CHECK(ui.m_namedTimerReadyBold == true);
}

void CheckAllBoundToB(const InGameUI& ui, DisplayString& message0, DisplayString& message1,
	SuperweaponInfo& normalSuperweapon, DisplayString& normalName, DisplayString& normalTime,
	SuperweaponInfo& readySuperweapon, DisplayString& readyName, DisplayString& readyTime,
	NamedTimerInfo& normalTimer, NamedTimerInfo& readyTimer, NamedTimerInfo& nonCountdownTimer,
	DisplayString& subtitleTitle, DisplayString& subtitleComplete, DisplayString& subtitleCurrent,
	Drawable& firstDrawable, Drawable& secondDrawable)
{
	CheckEffectiveSettings(ui, "B", 31);
	CheckBound(&message0, "Message-B", 32, false);
	CheckBound(&message1, "Message-B", 32, false);
	CheckBound(&normalName, "Super-B", 35, true);
	CheckBound(&normalTime, "Super-B", 35, true);
	CheckBound(&readyName, "SuperReady-B", 36, false);
	CheckBound(&readyTime, "SuperReady-B", 36, false);
	CheckBound(normalTimer.displayString, "Timer-B", 37, true);
	CheckBound(readyTimer.displayString, "TimerReady-B", 38, false);
	CheckBound(nonCountdownTimer.displayString, "Timer-B", 37, true);
	CheckBound(&subtitleTitle, "Title-B", 33, true);
	CheckBound(&subtitleComplete, "Body-B", 34, false);
	CheckBound(&subtitleCurrent, "Body-B", 34, false);
	CheckBound(firstDrawable.m_constructDisplayString, "Caption-B", 31, true);
	CheckBound(firstDrawable.m_captionDisplayString, "Caption-B", 31, true);
	CheckBound(secondDrawable.m_constructDisplayString, "Caption-B", 31, true);
	CHECK(secondDrawable.m_captionDisplayString == nullptr);
	CHECK(normalSuperweapon.m_ready == false);
	CHECK(readySuperweapon.m_ready == true);
}

void CheckAllBoundToIni(const InGameUI& ui, DisplayString& message0, DisplayString& message1,
	DisplayString& normalName, DisplayString& normalTime,
	DisplayString& readyName, DisplayString& readyTime,
	NamedTimerInfo& normalTimer, NamedTimerInfo& readyTimer, NamedTimerInfo& nonCountdownTimer,
	DisplayString& subtitleTitle, DisplayString& subtitleComplete, DisplayString& subtitleCurrent,
	Drawable& firstDrawable, Drawable& secondDrawable)
{
	CheckIniSettings(ui);
	CheckBound(&message0, "Message-INI", 10, true);
	CheckBound(&message1, "Message-INI", 10, true);
	CheckBound(&normalName, "Super-INI", 14, false);
	CheckBound(&normalTime, "Super-INI", 14, false);
	CheckBound(&readyName, "SuperReady-INI", 15, true);
	CheckBound(&readyTime, "SuperReady-INI", 15, true);
	CheckBound(normalTimer.displayString, "Timer-INI", 16, false);
	CheckBound(readyTimer.displayString, "TimerReady-INI", 17, true);
	CheckBound(nonCountdownTimer.displayString, "Timer-INI", 16, false);
	CheckBound(&subtitleTitle, "Title-INI", 12, false);
	CheckBound(&subtitleComplete, "Body-INI", 13, true);
	CheckBound(&subtitleCurrent, "Body-INI", 13, true);
	CheckBound(firstDrawable.m_constructDisplayString, "Caption-INI", 9, false);
	CheckBound(firstDrawable.m_captionDisplayString, "Caption-INI", 9, false);
	CheckBound(secondDrawable.m_constructDisplayString, "Caption-INI", 9, false);
}

void CheckPersistentState(const DisplayString& message0, const DisplayString& message1,
	const SuperweaponInfo& normalSuperweapon, const DisplayString& normalName,
	const DisplayString& normalTime, const SuperweaponInfo& readySuperweapon,
	const DisplayString& readyName, const DisplayString& readyTime,
	const NamedTimerInfo& normalTimer, const NamedTimerInfo& readyTimer,
	const NamedTimerInfo& nonCountdownTimer, const DisplayString& subtitleTitle,
	const DisplayString& subtitleComplete, const DisplayString& subtitleCurrent,
	const MilitarySubtitle& subtitle, const Drawable& firstDrawable,
	const Drawable& secondDrawable)
{
	CHECK(message0.text == "message one");
	CHECK(message1.text == "message two");
	CHECK(normalName.text == "normal weapon");
	CHECK(normalTime.text == "normal time");
	CHECK(readyName.text == "ready weapon");
	CHECK(readyTime.text == "ready time");
	CHECK(normalSuperweapon.m_timestamp == 111);
	CHECK(normalSuperweapon.m_ready == false);
	CHECK(readySuperweapon.m_timestamp == 222);
	CHECK(readySuperweapon.m_ready == true);
	CHECK(normalTimer.timerText.value == "normal timer");
	CHECK(normalTimer.timestamp == 60 && normalTimer.isCountdown);
	CHECK(readyTimer.timerText.value == "ready timer");
	CHECK(readyTimer.timestamp == 0 && readyTimer.isCountdown);
	CHECK(nonCountdownTimer.timerText.value == "plain timer");
	CHECK(nonCountdownTimer.timestamp == 0 && !nonCountdownTimer.isCountdown);
	CHECK(subtitleTitle.text == "subtitle title");
	CHECK(subtitleComplete.text == "completed line");
	CHECK(subtitleCurrent.text == "partially typed");
	CHECK(subtitle.currentDisplayString == 2);
	CHECK(subtitle.position.x == 23 && subtitle.position.y == 31);
	CHECK(firstDrawable.m_constructDisplayString->text == "construct 27%");
	CHECK(firstDrawable.m_captionDisplayString->text == "selected object");
	CHECK(secondDrawable.m_constructDisplayString->text == "construct 63%");
	CHECK(secondDrawable.m_captionDisplayString == nullptr);
}

void ResetMeasurements(GlobalLanguageData& language, FontLibrary& fonts)
{
	language.adjustedInputs.clear();
	fonts.lookupCount = 0;
}

Int RunLocalizedUIFontRefreshTests()
{
	GlobalLanguageData language;
	FontLibrary fonts;
	InGameUI ui;
	ConfigureIniFallback(ui);
	SetLanguageDescriptors(language, "A", 20);
	TheInGameUI = &ui;
	TheGlobalLanguageData = &language;
	TheFontLibrary = &fonts;

	TheInGameUI = &ui;
	SetLanguageDescriptors(language, "A", 20);
	ui.captureIniFontSettings();
	ui.applyLocalizedFontSettings();

	DisplayString message0("message one"), message1("message two");
	Bind(message0, fonts, language, ui.m_messageFont, ui.m_messagePointSize, ui.m_messageBold);
	Bind(message1, fonts, language, ui.m_messageFont, ui.m_messagePointSize, ui.m_messageBold);
	ui.m_uiMessages[0].displayString = &message0;
	ui.m_uiMessages[1].displayString = &message1;

	DisplayString normalName("normal weapon"), normalTime("normal time");
	DisplayString readyName("ready weapon"), readyTime("ready time");
	Bind(normalName, fonts, language, ui.m_superweaponNormalFont,
		ui.m_superweaponNormalPointSize, ui.m_superweaponNormalBold);
	Bind(normalTime, fonts, language, ui.m_superweaponNormalFont,
		ui.m_superweaponNormalPointSize, ui.m_superweaponNormalBold);
	Bind(readyName, fonts, language, ui.m_superweaponReadyFont,
		ui.m_superweaponReadyPointSize, ui.m_superweaponReadyBold);
	Bind(readyTime, fonts, language, ui.m_superweaponReadyFont,
		ui.m_superweaponReadyPointSize, ui.m_superweaponReadyBold);
	SuperweaponInfo normalSuperweapon = {};
	normalSuperweapon.m_nameDisplayString = &normalName;
	normalSuperweapon.m_timeDisplayString = &normalTime;
	normalSuperweapon.m_ready = false;
	normalSuperweapon.m_timestamp = 111;
	SuperweaponInfo readySuperweapon = {};
	readySuperweapon.m_nameDisplayString = &readyName;
	readySuperweapon.m_timeDisplayString = &readyTime;
	readySuperweapon.m_ready = true;
	readySuperweapon.m_timestamp = 222;
	ui.m_superweapons[0][1].push_back(&normalSuperweapon);
	ui.m_superweapons[0][2].push_back(&readySuperweapon);

	DisplayString normalTimerString("normal timer"), readyTimerString("ready timer");
	DisplayString nonCountdownTimerString("plain timer");
	Bind(normalTimerString, fonts, language, ui.m_namedTimerNormalFont,
		ui.m_namedTimerNormalPointSize, ui.m_namedTimerNormalBold);
	Bind(readyTimerString, fonts, language, ui.m_namedTimerReadyFont,
		ui.m_namedTimerReadyPointSize, ui.m_namedTimerReadyBold);
	Bind(nonCountdownTimerString, fonts, language, ui.m_namedTimerNormalFont,
		ui.m_namedTimerNormalPointSize, ui.m_namedTimerNormalBold);
	NamedTimerInfo normalTimer;
	normalTimer.timerText = "normal timer";
	normalTimer.displayString = &normalTimerString;
	normalTimer.timestamp = 60;
	normalTimer.isCountdown = true;
	NamedTimerInfo readyTimer;
	readyTimer.timerText = "ready timer";
	readyTimer.displayString = &readyTimerString;
	readyTimer.timestamp = 0;
	readyTimer.isCountdown = true;
	NamedTimerInfo nonCountdownTimer;
	nonCountdownTimer.timerText = "plain timer";
	nonCountdownTimer.displayString = &nonCountdownTimerString;
	nonCountdownTimer.timestamp = 0;
	nonCountdownTimer.isCountdown = false;
	ui.m_namedTimers[AsciiString("normal")] = &normalTimer;
	ui.m_namedTimers[AsciiString("ready")] = &readyTimer;
	ui.m_namedTimers[AsciiString("plain")] = &nonCountdownTimer;

	DisplayString subtitleTitle("subtitle title"), subtitleComplete("completed line");
	DisplayString subtitleCurrent("partially typed");
	Bind(subtitleTitle, fonts, language, ui.m_militaryCaptionTitleFont,
		ui.m_militaryCaptionTitlePointSize, ui.m_militaryCaptionTitleBold);
	Bind(subtitleComplete, fonts, language, ui.m_militaryCaptionFont,
		ui.m_militaryCaptionPointSize, ui.m_militaryCaptionBold);
	Bind(subtitleCurrent, fonts, language, ui.m_militaryCaptionFont,
		ui.m_militaryCaptionPointSize, ui.m_militaryCaptionBold);
	MilitarySubtitle subtitle;
	subtitle.position = Point2(23, 31);
	subtitle.blockPos = Point2(100, 200);
	subtitle.currentDisplayString = 2;
	subtitle.displayStrings[0] = &subtitleTitle;
	subtitle.displayStrings[1] = &subtitleComplete;
	subtitle.displayStrings[2] = &subtitleCurrent;
	ui.m_militarySubtitle = &subtitle;

	DisplayString construct0("construct 27%"), caption0("selected object");
	DisplayString construct1("construct 63%");
	Drawable drawable0, drawable1;
	drawable0.next = &drawable1;
	drawable0.m_constructDisplayString = &construct0;
	drawable0.m_captionDisplayString = &caption0;
	drawable1.m_constructDisplayString = &construct1;
	BindDrawable(drawable0, fonts, language, ui);
	BindDrawable(drawable1, fonts, language, ui);
	GameClient gameClient;
	gameClient.head = &drawable0;
	TheGameClient = &gameClient;

	ResetMeasurements(language, fonts);
	SetLanguageDescriptors(language, "B", 31);
	ui.refreshLocalizedFontResources();
	CheckAllBoundToB(ui, message0, message1, normalSuperweapon, normalName, normalTime,
		readySuperweapon, readyName, readyTime, normalTimer, readyTimer, nonCountdownTimer,
		subtitleTitle, subtitleComplete, subtitleCurrent, drawable0, drawable1);
	CHECK(language.adjustedInputs.size() == 13);
	CHECK(fonts.lookupCount == 13);
	CHECK(gameClient.firstReadCount == 1);
	CHECK(drawable0.nextReadCount == 1 && drawable1.nextReadCount == 1);
	CHECK(subtitle.blockPos.y == 31 + (33 + 3 + 2) + (34 + 3 + 2));
	CHECK(subtitle.blockPos.x == 23 + (34 + 3) * static_cast<Int>(subtitleCurrent.text.size()));
	CheckPersistentState(message0, message1, normalSuperweapon, normalName, normalTime,
		readySuperweapon, readyName, readyTime, normalTimer, readyTimer,
		nonCountdownTimer, subtitleTitle, subtitleComplete, subtitleCurrent, subtitle,
		drawable0, drawable1);

	// A same-resolution decline/reapply is idempotent: source sizes stay logical, not scaled twice.
	ResetMeasurements(language, fonts);
	ui.refreshLocalizedFontResources();
	CheckAllBoundToB(ui, message0, message1, normalSuperweapon, normalName, normalTime,
		readySuperweapon, readyName, readyTime, normalTimer, readyTimer, nonCountdownTimer,
		subtitleTitle, subtitleComplete, subtitleCurrent, drawable0, drawable1);
	CHECK(ui.m_drawableCaptionPointSize == 31);
	CHECK(language.adjustedInputs.size() == 13 && fonts.lookupCount == 13);
	CHECK(subtitle.blockPos.y == 31 + (33 + 3 + 2) + (34 + 3 + 2));
	CHECK(subtitle.blockPos.x == 23 + (34 + 3) * static_cast<Int>(subtitleCurrent.text.size()));
	CheckPersistentState(message0, message1, normalSuperweapon, normalName, normalTime,
		readySuperweapon, readyName, readyTime, normalTimer, readyTimer,
		nonCountdownTimer, subtitleTitle, subtitleComplete, subtitleCurrent, subtitle,
		drawable0, drawable1);

	// Missing localized descriptors restore the complete post-INI tuple and rebind existing strings.
	ResetMeasurements(language, fonts);
	ClearLanguageDescriptors(language);
	ui.refreshLocalizedFontResources();
	CheckAllBoundToIni(ui, message0, message1, normalName, normalTime, readyName, readyTime,
		normalTimer, readyTimer, nonCountdownTimer, subtitleTitle, subtitleComplete,
		subtitleCurrent, drawable0, drawable1);
	CHECK(language.adjustedInputs.size() == 13 && fonts.lookupCount == 13);
	CHECK(subtitle.blockPos.y == 31 + (12 + 3 + 2) + (13 + 3 + 2));
	CHECK(subtitle.blockPos.x == 23 + (13 + 3) * static_cast<Int>(subtitleCurrent.text.size()));
	CheckPersistentState(message0, message1, normalSuperweapon, normalName, normalTime,
		readySuperweapon, readyName, readyTime, normalTimer, readyTimer,
		nonCountdownTimer, subtitleTitle, subtitleComplete, subtitleCurrent, subtitle,
		drawable0, drawable1);

	// The pre-capture and null-resource guards must not dereference or traverse UI resources.
	ResetMeasurements(language, fonts);
	InGameUI uninitialized;
	TheInGameUI = &uninitialized;
	uninitialized.refreshLocalizedFontResources();
	CHECK(language.adjustedInputs.empty());
	CHECK(fonts.lookupCount == 0);
	CHECK(gameClient.firstReadCount == 3);

	InGameUI nullLibrary;
	ConfigureIniFallback(nullLibrary);
	SetLanguageDescriptors(language, "A", 20);
	TheInGameUI = &nullLibrary;
	nullLibrary.captureIniFontSettings();
	nullLibrary.applyLocalizedFontSettings();
	DisplayString unchanged("font library guard");
	Bind(unchanged, fonts, language, nullLibrary.m_messageFont,
		nullLibrary.m_messagePointSize, nullLibrary.m_messageBold);
	nullLibrary.m_uiMessages[0].displayString = &unchanged;
	GameFont* priorFont = unchanged.font;
	ResetMeasurements(language, fonts);
	SetLanguageDescriptors(language, "B", 31);
	TheFontLibrary = nullptr;
	nullLibrary.refreshLocalizedFontResources();
	CHECK(nullLibrary.m_messageFont.value == "Message-B");
	CHECK(unchanged.font == priorFont && unchanged.text == "font library guard");
	CHECK(language.adjustedInputs.empty() && fonts.lookupCount == 0);
	CHECK(gameClient.firstReadCount == 3);
	TheFontLibrary = &fonts;

	InGameUI nullLanguage;
	ConfigureIniFallback(nullLanguage);
	TheInGameUI = &nullLanguage;
	nullLanguage.captureIniFontSettings();
	nullLanguage.applyLocalizedFontSettings();
	DisplayString nullLanguageString("language guard");
	Bind(nullLanguageString, fonts, language, nullLanguage.m_messageFont,
		nullLanguage.m_messagePointSize, nullLanguage.m_messageBold);
	nullLanguage.m_uiMessages[0].displayString = &nullLanguageString;
	priorFont = nullLanguageString.font;
	ResetMeasurements(language, fonts);
	TheGlobalLanguageData = nullptr;
	nullLanguage.refreshLocalizedFontResources();
	CheckIniSettings(nullLanguage);
	CHECK(nullLanguageString.font == priorFont && nullLanguageString.text == "language guard");
	CHECK(language.adjustedInputs.empty() && fonts.lookupCount == 0);
	TheGlobalLanguageData = &language;

	return static_cast<Int>(testFailures);
}
