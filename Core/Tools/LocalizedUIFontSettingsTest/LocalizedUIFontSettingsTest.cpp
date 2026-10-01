/*
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "Common/LocalizedUIFontSettings.h"

#include <stdio.h>
#include <string>

namespace
{

typedef LocalizedUIFontSettings<std::string, int, bool> TestFontSettings;

unsigned int failures = 0;

#define CHECK(condition) do { if (!(condition)) { ++failures; \
	fprintf(stderr, "line %u: %s\n", static_cast<unsigned>(__LINE__), \
		#condition); } } while (0)

bool SameSettings(const TestFontSettings& left, const TestFontSettings& right)
{
	return left.name == right.name && left.pointSize == right.pointSize &&
		left.bold == right.bold;
}

} // namespace

int main()
{
	const TestFontSettings iniFallback = { "INI font", 11, false };
	const TestFontSettings firstResolution = { "localized A", 16, true };
	const TestFontSettings secondResolution = { "localized B", 19, false };
	const TestFontSettings noLocalizedFont = { "", 0, false };

	TestFontSettings selected = ResolveLocalizedUIFontSettings(
		iniFallback, firstResolution, !firstResolution.name.empty());
	CHECK(SameSettings(selected, firstResolution));

	selected = ResolveLocalizedUIFontSettings(
		iniFallback, secondResolution, !secondResolution.name.empty());
	CHECK(SameSettings(selected, secondResolution));

	selected = ResolveLocalizedUIFontSettings(
		iniFallback, noLocalizedFont, !noLocalizedFont.name.empty());
	CHECK(SameSettings(selected, iniFallback));

	return failures == 0 ? 0 : 1;
}
