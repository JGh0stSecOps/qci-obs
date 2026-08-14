/******************************************************************************
    Copyright (C) 2023 by Dennis Sädtler <dennis@obsproject.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#pragma once

#include <QString>
#include <QStringList>

#include <filesystem>

/*
 * Appearance > Density.
 *
 * THESE ARE QButtonGroup AUTO IDS, NOT AN ORDINAL WE CHOSE. The four buttons in
 * QCiBasicSettings.ui are added to `appearanceDensityButtonGroup` without explicit ids, so Qt
 * assigns them by counting DOWN from -2 in declaration order, and SaveAppearanceSettings persists
 * whatever `checkedId()` hands back. -2..-5 are therefore the only values the UI can ever write,
 * and any other value in the config is one nothing on screen can produce.
 *
 * They are named here because they were previously spelled as bare negative literals inside
 * getPaddingForDensityId() while the default was set as a bare `1` in QCiApp.cpp — two copies of
 * one fact, in two files, that disagreed. A fresh profile matched no branch and silently took the
 * function's fallback; the Settings dialog looked correct only because QButtonGroup::button(1)
 * returns null and the .ui's own `checked` attribute was left standing on the same button the
 * fallback happened to correspond to. Two unrelated accidents agreeing is not a working feature.
 *
 * Order is tightest to loosest and getPaddingForDensityId() must keep it that way; the row of
 * buttons is labelled Classic / Compact / Normal / Comfortable from left to right.
 */
enum class OBSDensity : int {
	Classic = -2,
	Compact = -3,
	Normal = -4,
	Comfortable = -5,
};

/*
 * The density a profile that has never opened Settings runs at.
 *
 * The repair landed first as OBSDensity::Normal, which returns 4 — the exact value the broken
 * fallback was already handing out, so the whole resolved stylesheet came out byte-identical and
 * the mapping change could be reviewed on its own. A mapping repair and a taste change in one
 * commit is a change nobody can review, because any difference could be either.
 *
 * COMPACT is the taste change, made separately and on purpose. The rig is a control surface:
 * events/control.html sets --rad to 0 and packs its panes, and the application that drives it
 * should not be looser than the panel it drives. One integer rescales roughly forty derived sizes
 * at once, which is why it is worth having exactly one of.
 */
inline constexpr int OBS_DENSITY_DEFAULT = static_cast<int>(OBSDensity::Compact);

/*
 * The bug that started this, refused by the compiler from here on.
 *
 * `1` was a legal int, so nothing objected: not the config, not the button group, not
 * getPaddingForDensityId(), which simply fell through. The only reason it was ever found is that
 * somebody read the branches and the default in the same sitting. A default that is not one of
 * the four densities is now a build failure rather than a layout nobody chose.
 */
static_assert(OBS_DENSITY_DEFAULT == static_cast<int>(OBSDensity::Classic) ||
		      OBS_DENSITY_DEFAULT == static_cast<int>(OBSDensity::Compact) ||
		      OBS_DENSITY_DEFAULT == static_cast<int>(OBSDensity::Normal) ||
		      OBS_DENSITY_DEFAULT == static_cast<int>(OBSDensity::Comfortable),
	      "Appearance/Density defaults to a value getPaddingForDensityId() does not branch on, "
	      "so a fresh profile would silently take the fallback padding");

struct OBSTheme {
	/* internal name, must be unique */
	QString id;
	QString name;
	QString author;
	QString extends;

	/* First ancestor base theme */
	QString parent;
	/* Dependencies from root to direct ancestor */
	QStringList dependencies;
	/* File path */
	std::filesystem::path location;
	std::filesystem::path filename; /* Filename without extension */

	bool isDark;
	bool isVisible;      /* Whether it should be shown to the user */
	bool isBaseTheme;    /* Whether it is a "style" or variant */
	bool isHighContrast; /* Whether it is a high-contrast adjustment layer */

	bool usesFontScale = false; /* Whether the generated QSS uses the font scale option */
	bool usesDensity = false;   /* Whether the generated QSS uses the density option */
};
