/*
 * Minimal collaborators for the source-extracted GlobalLanguage reload test.
 * The extracted production methods and parser table are included beside these types.
 */

typedef int Int;
typedef unsigned int UnsignedInt;
typedef float Real;
typedef bool Bool;
enum { FALSE = false, TRUE = true };

struct AsciiString
{
	std::string value;
	AsciiString() {}
	AsciiString(const char* text) : value(text ? text : "") {}
	AsciiString(const std::string& text) : value(text) {}
	void clear() { value.clear(); }
	const char* str() const { return value.c_str(); }
	AsciiString& operator=(const char* text) { value = text ? text : ""; return *this; }
	AsciiString& operator=(const AsciiString& other) { value = other.value; return *this; }
	void format(const char* pattern, const char* language)
	{
		char buffer[256];
		std::snprintf(buffer, sizeof(buffer), pattern, language);
		value = buffer;
	}
};

struct FontDesc
{
	FontDesc();
	AsciiString name;
	Int size;
	Bool bold;
};

struct LookupListRec
{
	const char* name;
	Int value;
};
typedef const LookupListRec* ConstLookupListRecArray;

class INI;
typedef void (*INIFieldParseProc)(INI*, void*, void*, const void*);

struct FieldParse
{
	const char* token;
	INIFieldParseProc parse;
	const void* userData;
	Int offset;
};

enum INILoadType { INI_LOAD_OVERWRITE };

struct IniEntry
{
	std::string key;
	std::vector<std::string> tokens;
};

static std::vector<IniEntry> g_languageEntries;
static std::vector<std::string> g_languageEvents;
static std::vector<std::string> g_registeredLocalFonts;
static Real g_resolutionFontPreference = -1.0f;
static Bool g_hasFullviewportDat = FALSE;
static Int g_globalLanguageTestFailures = 0;
static const char* g_globalLanguageFixtureTitle = "unset";

static void Check(Bool condition, const char* expression, Int line)
{
	if (!condition)
	{
		++g_globalLanguageTestFailures;
		std::fprintf(stderr, "%s GlobalLanguage reload assertion failed at line %d: %s\n",
			g_globalLanguageFixtureTitle, line, expression);
	}
}

#ifndef GLOBAL_LANGUAGE_CHECK
#define GLOBAL_LANGUAGE_CHECK(condition) Check((condition), #condition, static_cast<Int>(__LINE__))
#endif

static Bool EqualsIgnoreCase(const char* left, const char* right)
{
	if (!left || !right)
		return left == right;
	while (*left && *right)
	{
		if (std::tolower(static_cast<unsigned char>(*left)) !=
			std::tolower(static_cast<unsigned char>(*right)))
			return FALSE;
		++left;
		++right;
	}
	return *left == *right;
}

class SubsystemInterface
{
public:
	SubsystemInterface() {}
	virtual ~SubsystemInterface() {}
	virtual void init() = 0;
	virtual void reset() = 0;
	virtual void update() = 0;
protected:
	AsciiString m_name;
};

class GlobalLanguage : public SubsystemInterface
{
public:
	enum ResolutionFontSizeMethod
	{
		ResolutionFontSizeMethod_Classic,
		ResolutionFontSizeMethod_ClassicNoCeiling,
		ResolutionFontSizeMethod_Strict,
		ResolutionFontSizeMethod_Balanced,
		ResolutionFontSizeMethod_Default = ResolutionFontSizeMethod_ClassicNoCeiling
	};

	typedef std::list<AsciiString> StringList;
	GlobalLanguage();
	virtual ~GlobalLanguage();
	virtual void init() override;
	void onResolutionChanged();
	virtual void reset() override;
	virtual void update() override {}
	void parseCustomDefinition();
	static void parseFontFileName(INI*, void*, void*, const void*);
	static void parseFontDesc(INI*, void*, void*, const void*);

	AsciiString m_unicodeFontName;
	AsciiString m_unicodeFontFileName;
	Bool m_useHardWrap;
	Int m_militaryCaptionSpeed;
	Int m_militaryCaptionDelayMS;
	FontDesc m_copyrightFont;
	FontDesc m_messageFont;
	FontDesc m_militaryCaptionTitleFont;
	FontDesc m_militaryCaptionFont;
	FontDesc m_superweaponCountdownNormalFont;
	FontDesc m_superweaponCountdownReadyFont;
	FontDesc m_namedTimerCountdownNormalFont;
	FontDesc m_namedTimerCountdownReadyFont;
	FontDesc m_drawableCaptionFont;
	FontDesc m_defaultWindowFont;
	FontDesc m_defaultDisplayStringFont;
	FontDesc m_tooltipFontName;
	FontDesc m_nativeDebugDisplay;
	FontDesc m_drawGroupInfoFont;
	FontDesc m_creditsTitleFont;
	FontDesc m_creditsPositionFont;
	FontDesc m_creditsNormalFont;
	Real m_resolutionFontSizeAdjustment;
	Real m_userResolutionFontSizeAdjustment;
	ResolutionFontSizeMethod m_resolutionFontSizeMethod;
	StringList m_localFonts;
};

extern GlobalLanguage* TheGlobalLanguageData;

class INI
{
public:
	INI() : m_tokenIndex(0) {}
	static void parseLanguageDefinition(INI*);
	static void parseAsciiString(INI*, void*, void*, const void*);
	static void parseInt(INI*, void*, void*, const void*);
	static void parseBool(INI*, void*, void*, const void*);
	static void parseReal(INI*, void*, void*, const void*);
	static void parseLookupList(INI*, void*, void*, const void*);
	UnsignedInt loadFileDirectory(AsciiString, INILoadType, void*);
	void initFromINI(void*, const FieldParse*);
	const char* getNextToken();
	AsciiString getNextAsciiString();
	AsciiString getNextQuotedAsciiString();
	static Int scanInt(const char* token) { return std::atoi(token); }
	static Real scanReal(const char* token) { return static_cast<Real>(std::atof(token)); }
	static Bool scanBool(const char* token)
	{
		return token && (EqualsIgnoreCase(token, "yes") || EqualsIgnoreCase(token, "true"));
	}
private:
	std::vector<std::string> m_tokens;
	std::size_t m_tokenIndex;
};

class OptionPreferences
{
public:
	Real getResolutionFontAdjustment() const
	{
		g_languageEvents.push_back("preference");
		return g_resolutionFontPreference;
	}
};

static AsciiString GetRegistryLanguage()
{
	return AsciiString("english");
}

static Int AddFontResource(const char* filename)
{
	g_languageEvents.push_back(std::string("add:") + filename);
	g_registeredLocalFonts.push_back(filename);
	return 1;
}

static Int RemoveFontResource(const char* filename)
{
	g_languageEvents.push_back(std::string("remove:") + filename);
	for (std::vector<std::string>::iterator it = g_registeredLocalFonts.begin();
		it != g_registeredLocalFonts.end(); ++it)
	{
		if (*it == filename)
		{
			g_registeredLocalFonts.erase(it);
			return 1;
		}
	}
	Check(FALSE, "removed local font was registered", __LINE__);
	return 0;
}

#ifndef DEBUG_ASSERTCRASH
#define DEBUG_ASSERTCRASH(condition, message) do { if (!(condition)) Check(FALSE, "DEBUG_ASSERTCRASH condition", __LINE__); } while (0)
#endif
#ifndef DEBUG_CRASH
#define DEBUG_CRASH(message) do { Check(FALSE, "unexpected DEBUG_CRASH", __LINE__); } while (0)
#endif

namespace addon
{
	static Bool HasFullviewportDat()
	{
		g_languageEvents.push_back("addon");
		return g_hasFullviewportDat;
	}
}

UnsignedInt INI::loadFileDirectory(AsciiString filename, INILoadType, void*)
{
	g_languageEvents.push_back(std::string("load:") + filename.value);
	parseLanguageDefinition(this);
	return 1;
}

void INI::initFromINI(void* instance, const FieldParse* parseTable)
{
	for (std::vector<IniEntry>::const_iterator entry = g_languageEntries.begin();
		entry != g_languageEntries.end(); ++entry)
	{
		const FieldParse* field = parseTable;
		while (field->token && entry->key != field->token)
			++field;
		if (!field->token)
		{
			Check(FALSE, "fixture key appears in the production Language parse table", __LINE__);
			continue;
		}
		m_tokens = entry->tokens;
		m_tokenIndex = 0;
		void* store = reinterpret_cast<char*>(instance) + field->offset;
		field->parse(this, instance, store, field->userData);
	}
}

const char* INI::getNextToken()
{
	if (m_tokenIndex >= m_tokens.size())
		return "";
	return m_tokens[m_tokenIndex++].c_str();
}

AsciiString INI::getNextAsciiString()
{
	return AsciiString(getNextToken());
}

AsciiString INI::getNextQuotedAsciiString()
{
	return AsciiString(getNextToken());
}

void INI::parseAsciiString(INI* ini, void*, void* store, const void*)
{
	*static_cast<AsciiString*>(store) = ini->getNextAsciiString();
}

void INI::parseInt(INI* ini, void*, void* store, const void*)
{
	*static_cast<Int*>(store) = scanInt(ini->getNextToken());
}

void INI::parseBool(INI* ini, void*, void* store, const void*)
{
	*static_cast<Bool*>(store) = scanBool(ini->getNextToken());
}

void INI::parseReal(INI* ini, void*, void* store, const void*)
{
	*static_cast<Real*>(store) = scanReal(ini->getNextToken());
}

void INI::parseLookupList(INI* ini, void*, void* store, const void* userData)
{
	const char* token = ini->getNextToken();
	ConstLookupListRecArray lookup = static_cast<ConstLookupListRecArray>(userData);
	for (; lookup->name; ++lookup)
	{
		if (EqualsIgnoreCase(token, lookup->name))
		{
			*static_cast<Int*>(store) = lookup->value;
			return;
		}
	}
	Check(FALSE, "fixture lookup token appears in the production lookup table", __LINE__);
}
