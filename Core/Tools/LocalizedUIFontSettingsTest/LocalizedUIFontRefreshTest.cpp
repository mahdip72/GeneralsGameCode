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

/*
 * The build-time extractor compiles the selected production method bodies from both title trees
 * against these focused collaborators. This exercises their state transitions but is not a full
 * product link and does not validate real font assets, atlas behavior, or rendered appearance.
 */
#define CHECK(condition) Check((condition), #condition, static_cast<Int>(__LINE__))

namespace GeneralsFixture
{
#include "LocalizedUIFontRefreshMockTypes.inl"
#include "LocalizedUIFontRefresh_Generals.inl"
#include "LocalizedUIFontRefreshAssertions.inl"
}

namespace GeneralsMDFixture
{
#include "LocalizedUIFontRefreshMockTypes.inl"
#include "LocalizedUIFontRefresh_GeneralsMD.inl"
#include "LocalizedUIFontRefreshAssertions.inl"
}

int main()
{
	const int generalsFailures = GeneralsFixture::RunLocalizedUIFontRefreshTests();
	const int generalsMdFailures = GeneralsMDFixture::RunLocalizedUIFontRefreshTests();
	return generalsFailures == 0 && generalsMdFailures == 0 ? 0 : 1;
}
