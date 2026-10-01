/*
 * Exercises the actual extracted GlobalLanguage constructor, reload/init path,
 * parser dispatch, font callbacks, and FontDesc constructor against a focused
 * partial-INI/archive and font-registration model.
 */

#include <sstream>

struct ReloadFontField
{
	const char* token;
	FontDesc GlobalLanguage::* member;
};

static const ReloadFontField g_reloadFontFields[] =
{
	{ "CopyrightFont", &GlobalLanguage::m_copyrightFont },
	{ "MessageFont", &GlobalLanguage::m_messageFont },
	{ "MilitaryCaptionTitleFont", &GlobalLanguage::m_militaryCaptionTitleFont },
	{ "MilitaryCaptionFont", &GlobalLanguage::m_militaryCaptionFont },
	{ "SuperweaponCountdownNormalFont", &GlobalLanguage::m_superweaponCountdownNormalFont },
	{ "SuperweaponCountdownReadyFont", &GlobalLanguage::m_superweaponCountdownReadyFont },
	{ "NamedTimerCountdownNormalFont", &GlobalLanguage::m_namedTimerCountdownNormalFont },
	{ "NamedTimerCountdownReadyFont", &GlobalLanguage::m_namedTimerCountdownReadyFont },
	{ "DrawableCaptionFont", &GlobalLanguage::m_drawableCaptionFont },
	{ "DefaultWindowFont", &GlobalLanguage::m_defaultWindowFont },
	{ "DefaultDisplayStringFont", &GlobalLanguage::m_defaultDisplayStringFont },
	{ "TooltipFontName", &GlobalLanguage::m_tooltipFontName },
	{ "NativeDebugDisplay", &GlobalLanguage::m_nativeDebugDisplay },
	{ "DrawGroupInfoFont", &GlobalLanguage::m_drawGroupInfoFont },
	{ "CreditsTitleFont", &GlobalLanguage::m_creditsTitleFont },
	{ "CreditsMinorTitleFont", &GlobalLanguage::m_creditsPositionFont },
	{ "CreditsNormalFont", &GlobalLanguage::m_creditsNormalFont }
};

static void AddLanguageEntry(std::vector<IniEntry>& entries, const char* key,
	const std::vector<std::string>& tokens)
{
	IniEntry entry;
	entry.key = key;
	entry.tokens = tokens;
	entries.push_back(entry);
}

static void AddLanguageEntry(std::vector<IniEntry>& entries, const char* key, const char* token)
{
	std::vector<std::string> tokens(1, token);
	AddLanguageEntry(entries, key, tokens);
}

static void AddFontEntry(std::vector<IniEntry>& entries, const char* key,
	const std::string& name, Int pointSize, Bool bold)
{
	std::vector<std::string> tokens;
	tokens.push_back(name);
	tokens.push_back(std::to_string(pointSize));
	tokens.push_back(bold ? "Yes" : "No");
	AddLanguageEntry(entries, key, tokens);
}

static std::vector<IniEntry> BuildCompleteDefinitionA()
{
	std::vector<IniEntry> entries;
	AddLanguageEntry(entries, "UnicodeFontName", "A-Unicode");
	AddLanguageEntry(entries, "MilitaryCaptionSpeed", "875");
	AddLanguageEntry(entries, "UseHardWordWrap", "Yes");
	AddLanguageEntry(entries, "ResolutionFontAdjustment", "1.35");
	AddLanguageEntry(entries, "ResolutionFontSizeMethod", "BALANCED");
	AddLanguageEntry(entries, "MilitaryCaptionDelayMS", "1430");
	AddLanguageEntry(entries, "LocalFontFile", "A-local-1.ttf");
	AddLanguageEntry(entries, "LocalFontFile", "A-local-2.ttf");
	for (std::size_t i = 0; i < sizeof(g_reloadFontFields) / sizeof(g_reloadFontFields[0]); ++i)
	{
		std::ostringstream name;
		name << "A-Font-" << i;
		AddFontEntry(entries, g_reloadFontFields[i].token, name.str(),
			static_cast<Int>(20 + i), (i % 2) != 0);
	}
	return entries;
}

static std::vector<IniEntry> BuildPartialDefinitionB()
{
	std::vector<IniEntry> entries;
	AddFontEntry(entries, "MessageFont", "B-Message", 47, TRUE);
	AddLanguageEntry(entries, "LocalFontFile", "B-local.ttf");
	return entries;
}

static void CheckFont(const FontDesc& font, const std::string& expectedName,
	Int expectedSize, Bool expectedBold, const char* fieldName)
{
	GLOBAL_LANGUAGE_CHECK(font.name.value == expectedName);
	GLOBAL_LANGUAGE_CHECK(font.size == expectedSize);
	GLOBAL_LANGUAGE_CHECK(font.bold == expectedBold);
	if (font.name.value != expectedName)
		std::fprintf(stderr, "%s %s name mismatch: got '%s', expected '%s'\n",
			g_globalLanguageFixtureTitle, fieldName, font.name.str(), expectedName.c_str());
}

static void CheckAllFontDefaults(const GlobalLanguage& language)
{
	for (std::size_t i = 0; i < sizeof(g_reloadFontFields) / sizeof(g_reloadFontFields[0]); ++i)
		CheckFont(language.*(g_reloadFontFields[i].member), "Arial Unicode MS", 12, FALSE,
			g_reloadFontFields[i].token);
}

static void CheckDefinitionA(const GlobalLanguage& language, Real userPreference)
{
	GLOBAL_LANGUAGE_CHECK(language.m_unicodeFontName.value == "A-Unicode");
	GLOBAL_LANGUAGE_CHECK(language.m_unicodeFontFileName.value.empty());
	GLOBAL_LANGUAGE_CHECK(language.m_militaryCaptionSpeed == 875);
	GLOBAL_LANGUAGE_CHECK(language.m_useHardWrap == TRUE);
	GLOBAL_LANGUAGE_CHECK(std::fabs(language.m_resolutionFontSizeAdjustment - 1.35f) < 0.001f);
	GLOBAL_LANGUAGE_CHECK(language.m_resolutionFontSizeMethod == GlobalLanguage::ResolutionFontSizeMethod_Balanced);
	GLOBAL_LANGUAGE_CHECK(language.m_militaryCaptionDelayMS == 1430);
	GLOBAL_LANGUAGE_CHECK(std::fabs(language.m_userResolutionFontSizeAdjustment - userPreference) < 0.001f);
	for (std::size_t i = 0; i < sizeof(g_reloadFontFields) / sizeof(g_reloadFontFields[0]); ++i)
	{
		std::ostringstream expectedName;
		expectedName << "A-Font-" << i;
		CheckFont(language.*(g_reloadFontFields[i].member), expectedName.str(),
			static_cast<Int>(20 + i), (i % 2) != 0, g_reloadFontFields[i].token);
	}
}

static void CheckDefinitionBDefaults(const GlobalLanguage& language, Real userPreference,
	GlobalLanguage::ResolutionFontSizeMethod expectedMethod)
{
	GLOBAL_LANGUAGE_CHECK(language.m_unicodeFontName.value.empty());
	GLOBAL_LANGUAGE_CHECK(language.m_unicodeFontFileName.value.empty());
	GLOBAL_LANGUAGE_CHECK(language.m_militaryCaptionSpeed == 0);
	GLOBAL_LANGUAGE_CHECK(language.m_useHardWrap == FALSE);
	GLOBAL_LANGUAGE_CHECK(std::fabs(language.m_resolutionFontSizeAdjustment - 0.7f) < 0.001f);
	GLOBAL_LANGUAGE_CHECK(language.m_resolutionFontSizeMethod == expectedMethod);
	GLOBAL_LANGUAGE_CHECK(language.m_militaryCaptionDelayMS == 750);
	GLOBAL_LANGUAGE_CHECK(std::fabs(language.m_userResolutionFontSizeAdjustment - userPreference) < 0.001f);
	for (std::size_t i = 0; i < sizeof(g_reloadFontFields) / sizeof(g_reloadFontFields[0]); ++i)
	{
		if (std::strcmp(g_reloadFontFields[i].token, "MessageFont") == 0)
			CheckFont(language.*(g_reloadFontFields[i].member), "B-Message", 47, TRUE, "MessageFont");
		else
			CheckFont(language.*(g_reloadFontFields[i].member), "Arial Unicode MS", 12, FALSE,
				g_reloadFontFields[i].token);
	}
}

static std::vector<std::string> CurrentLanguageFontNames(const GlobalLanguage& language)
{
	std::vector<std::string> names;
	for (GlobalLanguage::StringList::const_iterator it = language.m_localFonts.begin();
		it != language.m_localFonts.end(); ++it)
		names.push_back(it->value);
	return names;
}

static void CheckEvents(const std::vector<std::string>& expected)
{
	GLOBAL_LANGUAGE_CHECK(g_languageEvents == expected);
	if (g_languageEvents != expected)
	{
		std::fprintf(stderr, "%s event sequence mismatch; observed:", g_globalLanguageFixtureTitle);
		for (std::vector<std::string>::const_iterator it = g_languageEvents.begin();
			it != g_languageEvents.end(); ++it)
			std::fprintf(stderr, " %s", it->c_str());
		std::fprintf(stderr, "\n");
	}
}

static std::vector<std::string> ExpectedReloadEvents(
	const std::vector<std::string>& oldFonts,
	const std::vector<std::string>& newFonts)
{
	std::vector<std::string> expected;
	for (std::vector<std::string>::const_iterator it = oldFonts.begin(); it != oldFonts.end(); ++it)
		expected.push_back(std::string("remove:") + *it);
	expected.push_back("load:Data\\english\\Language");
	for (std::vector<std::string>::const_iterator it = newFonts.begin(); it != newFonts.end(); ++it)
		expected.push_back(std::string("add:") + *it);
	expected.push_back("preference");
	expected.push_back("addon");
	return expected;
}

static std::string DefinitionFingerprint(const GlobalLanguage& language)
{
	std::ostringstream value;
	value << language.m_unicodeFontName.value << '|' << language.m_unicodeFontFileName.value << '|'
		<< language.m_militaryCaptionSpeed << '|' << language.m_useHardWrap << '|'
		<< language.m_militaryCaptionDelayMS << '|' << language.m_resolutionFontSizeAdjustment << '|'
		<< language.m_userResolutionFontSizeAdjustment << '|' << language.m_resolutionFontSizeMethod;
	for (std::size_t i = 0; i < sizeof(g_reloadFontFields) / sizeof(g_reloadFontFields[0]); ++i)
	{
		const FontDesc& font = language.*(g_reloadFontFields[i].member);
		value << '|' << font.name.value << ':' << font.size << ':' << font.bold;
	}
	for (GlobalLanguage::StringList::const_iterator it = language.m_localFonts.begin();
		it != language.m_localFonts.end(); ++it)
		value << "|local:" << it->value;
	return value.str();
}

static Int RunGlobalLanguageReloadTests(const char* title)
{
	g_globalLanguageFixtureTitle = title;
	g_globalLanguageTestFailures = 0;
	g_languageEvents.clear();
	g_registeredLocalFonts.clear();
	g_hasFullviewportDat = FALSE;
	g_resolutionFontPreference = 0.85f;
	g_languageEntries = BuildCompleteDefinitionA();

	{
		GlobalLanguage language;
		GlobalLanguage* const stableInstance = &language;
		TheGlobalLanguageData = stableInstance;
		CheckAllFontDefaults(language);
		GLOBAL_LANGUAGE_CHECK(language.m_unicodeFontName.value.empty());
		GLOBAL_LANGUAGE_CHECK(language.m_militaryCaptionSpeed == 0);
		GLOBAL_LANGUAGE_CHECK(language.m_useHardWrap == FALSE);
		GLOBAL_LANGUAGE_CHECK(language.m_militaryCaptionDelayMS == 750);
		GLOBAL_LANGUAGE_CHECK(language.m_resolutionFontSizeMethod == GlobalLanguage::ResolutionFontSizeMethod_Default);
		GLOBAL_LANGUAGE_CHECK(std::fabs(language.m_resolutionFontSizeAdjustment - 0.7f) < 0.001f);
		GLOBAL_LANGUAGE_CHECK(std::fabs(language.m_userResolutionFontSizeAdjustment + 1.0f) < 0.001f);

		language.init();
		language.parseCustomDefinition();
		CheckDefinitionA(language, 0.85f);
		GLOBAL_LANGUAGE_CHECK(TheGlobalLanguageData == stableInstance);
		const std::vector<std::string> fontA = { "A-local-2.ttf", "A-local-1.ttf" };
		GLOBAL_LANGUAGE_CHECK(CurrentLanguageFontNames(language) == fontA);
		GLOBAL_LANGUAGE_CHECK(g_registeredLocalFonts == fontA);
		CheckEvents({ "load:Data\\english\\Language", "add:A-local-2.ttf", "add:A-local-1.ttf", "preference", "addon" });

		g_languageEntries = BuildPartialDefinitionB();
		g_resolutionFontPreference = 1.25f;
		g_hasFullviewportDat = FALSE;
		g_languageEvents.clear();
		language.onResolutionChanged();
		CheckDefinitionBDefaults(language, 1.25f,
			GlobalLanguage::ResolutionFontSizeMethod_Default);
		GLOBAL_LANGUAGE_CHECK(TheGlobalLanguageData == stableInstance);
		const std::vector<std::string> fontB = { "B-local.ttf" };
		GLOBAL_LANGUAGE_CHECK(CurrentLanguageFontNames(language) == fontB);
		GLOBAL_LANGUAGE_CHECK(g_registeredLocalFonts == fontB);
		CheckEvents(ExpectedReloadEvents(fontA, fontB));

		g_hasFullviewportDat = TRUE;
		g_languageEvents.clear();
		language.onResolutionChanged();
		CheckDefinitionBDefaults(language, 1.25f,
			GlobalLanguage::ResolutionFontSizeMethod_Classic);
		GLOBAL_LANGUAGE_CHECK(language.m_resolutionFontSizeMethod == GlobalLanguage::ResolutionFontSizeMethod_Classic);
		CheckEvents(ExpectedReloadEvents(fontB, fontB));

		g_languageEntries = BuildCompleteDefinitionA();
		g_resolutionFontPreference = 0.85f;
		g_hasFullviewportDat = FALSE;
		g_languageEvents.clear();
		language.onResolutionChanged();
		CheckDefinitionA(language, 0.85f);
		GLOBAL_LANGUAGE_CHECK(TheGlobalLanguageData == stableInstance);
		GLOBAL_LANGUAGE_CHECK(CurrentLanguageFontNames(language) == fontA);
		GLOBAL_LANGUAGE_CHECK(g_registeredLocalFonts == fontA);
		CheckEvents(ExpectedReloadEvents(fontB, fontA));

		const std::string beforeSameDefinitionReload = DefinitionFingerprint(language);
		g_languageEvents.clear();
		language.onResolutionChanged();
		CheckDefinitionA(language, 0.85f);
		GLOBAL_LANGUAGE_CHECK(DefinitionFingerprint(language) == beforeSameDefinitionReload);
		GLOBAL_LANGUAGE_CHECK(TheGlobalLanguageData == stableInstance);
		GLOBAL_LANGUAGE_CHECK(CurrentLanguageFontNames(language) == fontA);
		GLOBAL_LANGUAGE_CHECK(g_registeredLocalFonts == fontA);
		CheckEvents(ExpectedReloadEvents(fontA, fontA));

		g_languageEvents.clear();
		TheGlobalLanguageData = nullptr;
	}

	GLOBAL_LANGUAGE_CHECK(g_registeredLocalFonts.empty());
	CheckEvents({ "remove:A-local-2.ttf", "remove:A-local-1.ttf" });
	return g_globalLanguageTestFailures;
}
