/*
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#ifndef LOCALIZED_UI_FONT_SETTINGS_H
#define LOCALIZED_UI_FONT_SETTINGS_H

template <typename NameType, typename PointSizeType, typename BoldType>
struct LocalizedUIFontSettings
{
	NameType name;
	PointSizeType pointSize;
	BoldType bold;
};

template <typename FontSettings>
inline FontSettings ResolveLocalizedUIFontSettings(
	const FontSettings& iniFallback,
	const FontSettings& localizedSettings,
	bool hasLocalizedSettings)
{
	return hasLocalizedSettings ? localizedSettings : iniFallback;
}

#endif
