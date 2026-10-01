/*
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <stdexcept>
#include <string>
#include <vector>
#include <sstream>

/*
 * The same shared Core/GameClient production method bodies are compiled into
 * isolated title fixtures. This covers the common parser/reload contract; it
 * is not a product link or an archive/renderer/visual test.
 */
namespace GeneralsGlobalLanguageFixture
{
#include "GlobalLanguageReloadMockTypes.inl"
#include "GlobalLanguageReload_Generals.inl"
#include "GlobalLanguageReloadAssertions.inl"
}

namespace GeneralsMDGlobalLanguageFixture
{
#include "GlobalLanguageReloadMockTypes.inl"
#include "GlobalLanguageReload_GeneralsMD.inl"
#include "GlobalLanguageReloadAssertions.inl"
}

int main()
{
	const int generalsFailures = GeneralsGlobalLanguageFixture::RunGlobalLanguageReloadTests("Generals");
	const int generalsMdFailures = GeneralsMDGlobalLanguageFixture::RunGlobalLanguageReloadTests("GeneralsMD");
	if (generalsFailures == 0 && generalsMdFailures == 0)
	{
		std::puts("GlobalLanguage partial-definition reload contract passed for both title fixtures.");
		return 0;
	}
	std::fprintf(stderr, "GlobalLanguage reload fixture failures: Generals=%d GeneralsMD=%d\n",
		generalsFailures, generalsMdFailures);
	return 1;
}
