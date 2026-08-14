/******************************************************************************
    QCi Studio — native docks for the QCi rig operator panel.

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
#include <Qt>

class OBSBasic;
class QDockWidget;
class QMenu;
class QWidget;

/*
 * THE OPERATOR PANEL — first-party docks, native Qt widgets, one model.
 *
 * The operator's brief for the fork was literal: "Now that we forked we no longer need browser
 * docks. Build it in first class in the source code." This file is the registration, layout and
 * safety half of that; docks/QCiRigPanes.* is the widgets and docks/QCiRigClient.* is the single
 * client they all render from.
 *
 * WHAT CHANGED FROM THE BROWSER-BACKED VERSION, AND WHY THE TABLE STILL POINTS AT control.html.
 * Each pane used to be a QCefWidget on `events/control.html?tab=…&bare=1`. The panes are real
 * widgets now, so the `spec` column is no longer a URL parameter — it is PROVENANCE. It names the
 * region(s) of the 7" console each native pane reproduces, which is what keeps the two surfaces
 * checkable against each other: qci-rig/lib/studiodocks.test.mjs reads that column, finds those
 * regions in control.html, and derives the safety rule below from the console's own source rather
 * than from a list somebody typed.
 */
namespace QCiRigDocks {

/* Which docks may share a tab bar. GROUP_NEVER means "always its own visible pane".
 *
 * ⚠️ SAFETY RULE, AND IT IS NOT A STYLE PREFERENCE. A tabified dock that is not the current tab is
 * INVISIBLE — Qt gives it no sliver of chrome, no ghost, nothing. Any pane whose hidden state can
 * let the operator act wrongly, or fail to act, must never be tabified. That covers the go-live /
 * transport controls first and foremost, and it also covers the privacy-relevant state this rig is
 * built around. See Tabifiable(); the rule is enforced there rather than by the table being typed
 * correctly, and qci-rig/lib/studiodocks.test.mjs derives WHICH pane holds the stream controls by
 * reading BOTH control.html and this fork's own pane source, so moving that button fails the test
 * instead of silently unprotecting a dock.
 */
enum RigDockGroup {
	GROUP_NEVER = -1, /* never tabified — see the warning above */
	GROUP_PANEL = 0,  /* the audience-facing panes: queue, reactions, chat, tank, audio */
};

struct RigDock {
	const char *objectName;   /* Qt saves and restores dock layout BY THIS NAME. Never reuse one. */
	const char *pane;         /* which native pane class MakeRigDockContent() builds */
	const char *spec;         /* space-separated control.html tab names this pane reproduces */
	const char *localeKey;    /* dock title, en-US.ini */
	Qt::DockWidgetArea area;  /* where it lands in a fresh layout */
	bool visibleByDefault;    /* shown on a fresh profile */
	bool dangerousWhenHidden; /* never tabified, never closable — see RigDockGroup and Load() */
	int group;                /* RigDockGroup */
};

/* The one predicate that decides whether a dock may share a tab bar.
 *
 * constexpr ON PURPOSE. It is asked at runtime in ApplyDefaultLayout(), but it is ALSO asked at
 * compile time by the table-walking static_assert in QCiRigDocks.cpp, and that assertion is the
 * obligation that transferred here when controlsDock was deleted: BUILTIN_DOCKS used to carry the
 * only dangerousWhenHidden row in the application, and QCiBasic_Docks.cpp asserted by name that
 * "controlsDock holds Go Live and must never be tabifiable". GO LIVE now lives on
 * qciRigControlDock, so the gate has to live on this table or the safety property is dropped in
 * the same commit that cleans up the other one. A runtime-only predicate cannot be asserted. */
constexpr bool Tabifiable(const RigDock &spec)
{
	if (spec.dangerousWhenHidden) {
		return false;
	}
	return spec.group != GROUP_NEVER;
}

/* ── the base URL has exactly one home ──────────────────────────────────────────────────────────
 * Config: section "QCiRig", key "BaseUrl", in the fork's USER config
 * (~/Library/Application Support/qci-studio/user.ini). Default: QCI_RIG_BASE_URL_DEFAULT in
 * QCiRigDocks.cpp — one constant, one accessor, and nothing else in the frontend may spell the
 * rig's address. QCiRigClient asks for it here rather than carrying its own.
 *
 * ⚠️ THE ADDRESS IS LOOPBACK-ONLY AND THAT IS A TRUST BOUNDARY, NOT A DEFAULT. See the long note in
 * QCiRigClient.hpp: this fork gets the routes the LAN panels are refused because it is on the same
 * machine as the rig. Pointing it off-box does not "connect to a remote rig", it points PRIVACY
 * HOLD at a stranger — and a stranger that answers anything at all reads as success. PromptForBaseUrl()
 * enforces it on the way in; SetBaseUrl() writes whatever it is given, so callers validate first.
 *
 * SetBaseUrl() returns false when the setting could not be written to disk. The value is live for
 * this session either way, so ignoring the result means the panel talks to one rig now and a
 * different one after the next restart, with nothing said. */
QString DefaultBaseUrl();
QString BaseUrl();
bool SetBaseUrl(const QString &url);

/* THE SEAM. One function builds every dock body, so a pane can be added, replaced or removed
 * without touching registration, layout, menus or config. Returns nullptr when no content can be
 * built, in which case the dock is not registered — an empty named dock in the Docks menu is worse
 * than a missing one, because it looks like a feature that is broken rather than one that is off. */
QWidget *MakeRigDockContent(QDockWidget *dock, const RigDock &spec);

/* Register every dock and add its toggle to the Docks menu. Call once, from OBSInit, before the
 * saved DockState is restored. `docksMenu` is OBSBasic's ui->menuDocks. */
void Load(OBSBasic *main, QMenu *docksMenu);

/* Place, size and tabify the docks as the fork's default layout. Idempotent. */
void ApplyDefaultLayout(OBSBasic *main);

/* ApplyDefaultLayout(), but only the first time this profile has ever seen these docks. Call after
 * the saved DockState has been restored: an existing profile's saved state has no opinion about a
 * dock that did not exist when it was written, so without this the operator's own profile would
 * come up with unplaced docks stacked in one corner. */
void ApplyDefaultLayoutIfNeeded(OBSBasic *main);

/* ⚠️ FORCE EVERY UNDISMISSABLE DOCK BACK ON THE GLASS. Call UNCONDITIONALLY after restoreState().
 *
 * A saved DockState is a file the operator's previous session wrote, and a profile written before
 * this dock was undismissable can carry it as hidden — at which point restoreState() faithfully
 * restores a rig with no visible privacy control and nothing ever puts it back, because
 * ApplyDefaultLayoutIfNeeded() has already fired for that profile. The panic control's visibility
 * cannot be a thing a saved file gets a vote on. */
void EnsureUndismissableVisible(OBSBasic *main);

/* Ask the operator for the panel's base URL. Wired to a Docks menu item. */
void PromptForBaseUrl(OBSBasic *main);

} // namespace QCiRigDocks
