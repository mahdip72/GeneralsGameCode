/*
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "Common/LocalizedUIFontSettings.h"

#include <cstdio>
#include <list>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#define LOCALIZED_UI_FONT_STALE_BASELINE_METHOD
#define CHECK(condition) Check((condition), #condition, __LINE__)

static int qualificationFailures = 0;

void Check(bool condition, const char* expression, int line)
{
	if (!condition)
	{
		++qualificationFailures;
		std::fprintf(stderr, "line %d: %s\n", line, expression);
	}
}

namespace GeneralsBaselineFixture
{
#include "LocalizedUIFontRefreshMockTypes.inl"
#include "LocalizedUIFontStaleBaseline_Generals.inl"
}

namespace GeneralsMDBaselineFixture
{
#include "LocalizedUIFontRefreshMockTypes.inl"
#include "LocalizedUIFontStaleBaseline_GeneralsMD.inl"
}

struct GeneralsBaselineAdapter
{
	typedef GeneralsBaselineFixture::GlobalLanguageData GlobalLanguageData;
	typedef GeneralsBaselineFixture::FontLibrary FontLibrary;
	typedef GeneralsBaselineFixture::InGameUI InGameUI;
	typedef GeneralsBaselineFixture::DisplayString DisplayString;
	typedef GeneralsBaselineFixture::GameFont GameFont;
	static void SetLanguageData(GlobalLanguageData* language)
	{
		GeneralsBaselineFixture::TheGlobalLanguageData = language;
	}
};

struct GeneralsMDBaselineAdapter
{
	typedef GeneralsMDBaselineFixture::GlobalLanguageData GlobalLanguageData;
	typedef GeneralsMDBaselineFixture::FontLibrary FontLibrary;
	typedef GeneralsMDBaselineFixture::InGameUI InGameUI;
	typedef GeneralsMDBaselineFixture::DisplayString DisplayString;
	typedef GeneralsMDBaselineFixture::GameFont GameFont;
	static void SetLanguageData(GlobalLanguageData* language)
	{
		GeneralsMDBaselineFixture::TheGlobalLanguageData = language;
	}
};

template <typename Fixture>
int RunStaleBaselineQualification()
{
	typename Fixture::GlobalLanguageData language;
	typename Fixture::FontLibrary fonts;
	typename Fixture::InGameUI ui;
	ui.m_drawableCaptionFont = "Caption-INI";
	ui.m_drawableCaptionPointSize = 9;
	ui.m_drawableCaptionBold = false;
	ui.m_messageFont = "Message-INI";
	ui.m_messagePointSize = 10;
	ui.m_messageBold = true;
	Fixture::SetLanguageData(&language);

	language.m_drawableCaptionFont.name = "Caption-A";
	language.m_drawableCaptionFont.size = 20;
	language.m_drawableCaptionFont.bold = false;
	language.m_messageFont.name = "Message-A";
	language.m_messageFont.size = 21;
	language.m_messageFont.bold = true;
	ui.applyOriginalLocalizedFontOverride();

	typename Fixture::DisplayString message("baseline message");
	message.setFont(fonts.getFont(ui.m_messageFont,
		language.adjustFontSize(ui.m_messagePointSize), ui.m_messageBold));
	typename Fixture::DisplayString construction("baseline construction");
	construction.setFont(fonts.getFont(ui.m_drawableCaptionFont,
		language.adjustFontSize(ui.m_drawableCaptionPointSize), ui.m_drawableCaptionBold));
	typename Fixture::GameFont* oldMessageFont = message.font;
	typename Fixture::GameFont* oldConstructionFont = construction.font;

	// The old path only applies descriptors during initialization; changing the language data
	// alone leaves both the UI tuple and already-bound strings on A.
	language.m_messageFont.name = "Message-B";
	language.m_messageFont.size = 31;
	language.m_messageFont.bold = false;
	language.m_drawableCaptionFont.name = "Caption-B";
	language.m_drawableCaptionFont.size = 30;
	language.m_drawableCaptionFont.bold = true;
	CHECK(ui.m_messageFont.value == "Message-A");
	CHECK(ui.m_drawableCaptionFont.value == "Caption-A");
	CHECK(message.font == oldMessageFont && message.font->name == "Message-A");
	CHECK(construction.font == oldConstructionFont && construction.font->name == "Caption-A");
	CHECK(message.fontSetCount == 1 && construction.fontSetCount == 1);
	return 0;
}

int main()
{
	RunStaleBaselineQualification<GeneralsBaselineAdapter>();
	RunStaleBaselineQualification<GeneralsMDBaselineAdapter>();
	return qualificationFailures == 0 ? 0 : 1;
}
