/*
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <cstdio>
#include <map>
#include <string>
#include <vector>

/*
 * Build-time extraction supplies the complete production DeclineResolution body and the exact
 * resolution-selection tail from saveOptions for each title. The collaborators below model only
 * the callback effects needed to assert sequencing and effective font-size reversion; they do not
 * link the full UI, verify font assets/atlases, or prove rendered appearance. The separate
 * LocalizedUIFontRefresh test extracts the production localized resource-refresh bodies.
 */

namespace GeneralsResolutionRefreshFixture
{
#include "ResolutionRefreshMockTypes.inl"
#include "ResolutionRefreshCallbacks_Generals.inl"
#include "ResolutionRefreshCallbackAssertions.inl"
}

namespace GeneralsMDResolutionRefreshFixture
{
#include "ResolutionRefreshMockTypes.inl"
#include "ResolutionRefreshCallbacks_GeneralsMD.inl"
#include "ResolutionRefreshCallbackAssertions.inl"
}

int main()
{
	const int generalsFailures = GeneralsResolutionRefreshFixture::RunResolutionRefreshCallbackTests();
	const int generalsMdFailures = GeneralsMDResolutionRefreshFixture::RunResolutionRefreshCallbackTests();
	return generalsFailures == 0 && generalsMdFailures == 0 ? 0 : 1;
}
