/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>
                          Zachary Lund <admin@computerquip.com>
                          Philippe Groarke <philippe.groarke@gmail.com>

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

#include "QCiBasic.hpp"

#include <docks/QCiRigDocks.hpp>

#include <qt-wrappers.hpp>

/* ── THE ONE LIST OF BUILTIN DOCKS ──────────────────────────────────────────────────────────────
 *
 * Mirrors the RIG_DOCKS pattern in docks/QCiRigDocks.cpp, for the same reason: a table is one place
 * to change, and a predicate beats a promise. See OBSBasic::BuiltinDockId in QCiBasic.hpp for why
 * this replaced four hand-written copies of the same six names.
 *
 * ⚠️ THE TWO BUILD-TIME GATES, AND WHY IT TAKES BOTH.
 *
 *   1. The static_asserts below. The count one catches an enumerator INSERTED anywhere at or before
 *      BUILTIN_STATS, because that shifts BUILTIN_STATS and the count stops matching. It does NOT
 *      catch one APPENDED after BUILTIN_STATS — measured, not assumed: appending left the count
 *      assert satisfied (6 == 5 + 1) and only gate 2 fired. rows_match_ids() covers the third case,
 *      a table reordered without reordering the enum, which is the nastiest of the three because
 *      everything compiles and the wrong dock silently gets locked.
 *   2. -Wswitch on BuiltinDock(), which has no default label. THIS is the gate that catches every
 *      case including the appended one, verified by adding a seventh enumerator and watching the
 *      build fail with "error: enumeration value 'BUILTIN_PROBE_DELETE_ME' not handled in switch
 *      [-Werror,-Wswitch]". It is a hard error in this tree, not a warning:
 *      cmake/common/compiler_common.cmake:85-86 defaults CMAKE_COMPILE_WARNING_AS_ERROR ON when it
 *      is not already defined, and cmake/macos/xcode.cmake:155-157 turns it into
 *      GCC_TREAT_WARNINGS_AS_ERRORS=YES alongside GCC_WARN_CHECK_SWITCH_STATEMENTS=YES.
 *
 * (Do not assume the -W flags in _obs_clang_common_options are doing this. They are NOT applied on
 * macOS — cmake/macos/compilerconfig.cmake includes compiler_common but never passes that list to
 * add_compile_options. The teeth here are Xcode's own warning settings.)
 *
 * dangerousWhenHidden means exactly what it means in QCiRigDocks.hpp: a dock whose hidden state can
 * make the operator act wrongly. controlsDock holds Go Live, so it is never tabified and never
 * closable. statsDock is the opposite — nothing goes wrong when it is not on screen.
 */
namespace {

/* Which docks may share a tab bar. GROUP_NONE means "always its own visible pane". Same shape and
 * same meaning as RigDockGroup in QCiRigDocks.hpp — deliberately, so there is one idea here and not
 * two. The rig's groups and these are kept SEPARATE tab bars: the rig owns its right-hand column. */
enum BuiltinDockGroup {
	GROUP_NONE = -1,
	GROUP_SIDE = 0,   /* the left column: what you build a scene out of */
	GROUP_BOTTOM = 1, /* the bottom strip: what you check while it runs */
};

struct BuiltinDockSpec {
	OBSBasic::BuiltinDockId id;
	const char *objectName;   /* Qt saves and restores dock layout BY THIS NAME. Never reuse one. */
	bool dangerousWhenHidden; /* never tabified — see Tabifiable() */
	bool closable;            /* may the operator dismiss it when the layout is unlocked? */
	Qt::DockWidgetArea area;  /* where it lands in a fresh layout */
	int group;                /* BuiltinDockGroup */
	bool visibleByDefault;
};

/* clang-format off */
constexpr BuiltinDockSpec BUILTIN_DOCKS[] = {
	/*                              objectName         dangerous closable area                     group         visible */
	{OBSBasic::BUILTIN_SCENES,      "scenesDock",      false,    false,   Qt::LeftDockWidgetArea,  GROUP_SIDE,   true},
	{OBSBasic::BUILTIN_SOURCES,     "sourcesDock",     false,    false,   Qt::LeftDockWidgetArea,  GROUP_SIDE,   true},
	{OBSBasic::BUILTIN_MIXER,       "mixerDock",       false,    false,   Qt::BottomDockWidgetArea,GROUP_BOTTOM, true},
	{OBSBasic::BUILTIN_TRANSITIONS, "transitionsDock", false,    false,   Qt::BottomDockWidgetArea,GROUP_BOTTOM, true},
	/* ⚠️ Go Live lives here. Hidden, the operator cannot tell whether they are streaming — so it is
	 * GROUP_NONE, and Tabifiable() refuses it even if somebody later types a real group in. */
	{OBSBasic::BUILTIN_CONTROLS,    "controlsDock",    true,     false,   Qt::BottomDockWidgetArea,GROUP_NONE,   true},
	/* The one dock that was always dismissable, and the only reason `features` differs from
	 * `mainFeatures` in on_lockDocks_toggled at all. Nothing goes wrong when Stats is off — so it
	 * joins the bottom tab bar rather than floating loose as it used to. */
	{OBSBasic::BUILTIN_STATS,       "statsDock",       false,    true,    Qt::BottomDockWidgetArea,GROUP_BOTTOM, false},
};
/* clang-format on */

constexpr int BUILTIN_DOCK_N = int(sizeof(BUILTIN_DOCKS) / sizeof(BUILTIN_DOCKS[0]));

static_assert(BUILTIN_DOCK_N == int(OBSBasic::BUILTIN_STATS) + 1,
	      "BUILTIN_DOCKS must have one row per BuiltinDockId — add the row for the new dock");

constexpr bool rows_match_ids()
{
	for (int i = 0; i < BUILTIN_DOCK_N; i++) {
		if (int(BUILTIN_DOCKS[i].id) != i) {
			return false;
		}
	}
	return true;
}

static_assert(rows_match_ids(), "BUILTIN_DOCKS rows must be in BuiltinDockId order — row i must be id i");

/* The safety rule, as a mechanism rather than a promise — same shape as QCiRigDocks::Tabifiable().
 * Asked once, at the point of use, rather than trusting the table to have been typed correctly. */
constexpr bool Tabifiable(const BuiltinDockSpec &spec)
{
	if (spec.dangerousWhenHidden) {
		return false;
	}
	return spec.group != GROUP_NONE;
}

static_assert(!Tabifiable(BUILTIN_DOCKS[OBSBasic::BUILTIN_CONTROLS]),
	      "controlsDock holds Go Live and must never be tabifiable");

/* ⚠️ THE RULE AS A TEST, NOT AS A COMMENT — this is the requirement "nothing whose hidden state is
 * dangerous may be tabified" encoded so that breaking it cannot compile. It walks the whole table
 * rather than naming controlsDock, so a SECOND dangerous dock added later is covered without anyone
 * remembering to extend an assertion. */
constexpr bool no_dangerous_dock_is_tabifiable()
{
	for (int i = 0; i < BUILTIN_DOCK_N; i++) {
		if (BUILTIN_DOCKS[i].dangerousWhenHidden && Tabifiable(BUILTIN_DOCKS[i])) {
			return false;
		}
	}
	return true;
}

static_assert(no_dangerous_dock_is_tabifiable(),
	      "a dock marked dangerousWhenHidden was given a tab group — a tabified dock that is not "
	      "the current tab is INVISIBLE, so its hidden state can mislead the operator");

} // namespace

/* Defined here rather than beside the other OBSBasic methods so the switch sits next to the table
 * it has to agree with. No default label ON PURPOSE — see gate 2 above. */
QDockWidget *OBSBasic::BuiltinDock(BuiltinDockId id) const
{
	switch (id) {
	case BUILTIN_SCENES:
		return ui->scenesDock;
	case BUILTIN_SOURCES:
		return ui->sourcesDock;
	case BUILTIN_MIXER:
		return ui->mixerDock;
	case BUILTIN_TRANSITIONS:
		return ui->transitionsDock;
	case BUILTIN_CONTROLS:
		return controlsDock;
	case BUILTIN_STATS:
		return statsDock;
	}
	return nullptr;
}

void setupDockAction(QDockWidget *dock)
{
	QAction *action = dock->toggleViewAction();

	auto neverDisable = [action]() {
		QSignalBlocker block(action);
		action->setEnabled(true);
	};

	auto newToggleView = [dock](bool check) {
		QSignalBlocker block(dock);
		dock->setVisible(check);

		/* ⚠️ setVisible(true) ALONE IS A NO-OP FOR A TABIFIED DOCK THAT IS NOT THE CURRENT TAB.
		 *
		 * Upstream's version of this lambda is the setVisible() line and nothing else, and it
		 * is silently wrong the moment any dock shares a tab bar: Qt restores the dock into
		 * its tab group without selecting it, so the menu item ticks, the dock is "visible" by
		 * every API you might ask, and the operator sees absolutely nothing change. The user
		 * then clicks the menu item again, which un-ticks it, and now the dock really is gone.
		 *
		 * The fork tabifies the rig docks on purpose (see docks/QCiRigDocks.cpp), so this is
		 * on the main path here rather than being a curiosity. raise() is what makes a tab
		 * current; on a floating or already-current dock it is harmless.
		 */
		if (check) {
			dock->raise();
		}
	};

	// Replace the slot connected by default
	QObject::disconnect(action, &QAction::triggered, nullptr, 0);
	QObject::connect(action, &QAction::triggered, dock, newToggleView);

	// Make the action unable to be disabled
	QObject::connect(action, &QAction::enabledChanged, action, neverDisable);
}

void OBSBasic::on_resetDocks_triggered(bool force)
{
#ifdef BROWSER_AVAILABLE
	if ((extraDocks.size() || extraCustomDocks.size() || extraBrowserDocks.size()) && !force)
#else
	if ((extraDocks.size() || extraCustomDocks.size()) && !force)
#endif
	{
		QMessageBox::StandardButton button =
			OBSMessageBox::question(this, QTStr("ResetUIWarning.Title"), QTStr("ResetUIWarning.Text"));

		if (button == QMessageBox::No) {
			return;
		}
	}

#define RESET_DOCKLIST(dockList)                                                                               \
	for (int i = dockList.size() - 1; i >= 0; i--) {                                                       \
		dockList[i]->setVisible(true);                                                                 \
		dockList[i]->setFloating(true);                                                                \
		dockList[i]->move(frameGeometry().topLeft() + rect().center() - dockList[i]->rect().center()); \
		dockList[i]->setVisible(false);                                                                \
	}

	RESET_DOCKLIST(extraDocks)
	RESET_DOCKLIST(extraCustomDocks)
#ifdef BROWSER_AVAILABLE
	RESET_DOCKLIST(extraBrowserDocks)
#endif
#undef RESET_DOCKLIST

	/* Must pass the SAME version saveState() used to write it (QCiBasic.cpp), or restoreState()
	 * returns false and silently restores nothing — leaving this function to lay out docks on top
	 * of whatever the previous layout happened to be. */
	restoreState(startingDockLayout, QCI_DOCK_STATE_VERSION);
	ui->sideDocks->setChecked(true);

	int cx = width();
	int bottomDocksHeight = height();

	bottomDocksHeight = bottomDocksHeight * 225 / 1000;

	/* ── THE FORK'S LAYOUT, NOT UPSTREAM'S ──────────────────────────────────────────────────
	 *
	 * Upstream stands all six docks side by side, and every one of them pays for its own title
	 * bar before it shows a single row. Scenes and Sources are the worst of it: two lists that
	 * are read together, each spending its own chrome, in the narrowest column on screen.
	 *
	 * So the docks that are read ALTERNATELY share a tab bar, and the docks that must be read
	 * AT A GLANCE do not. Scenes+Sources become one side pane; Mixer+Transitions+Stats become
	 * one bottom pane; Controls stays on its own because it holds Go Live, and a tabified dock
	 * that is not the current tab is not small — it is GONE. That rule is Tabifiable(), and it
	 * is enforced by static_assert above rather than by this loop being written correctly.
	 *
	 * Pass order matters and mirrors QCiRigDocks::ApplyDefaultLayout(): home, then tabify, then
	 * set visibility (so a dock meant to be hidden does not leave a live tab behind it), then
	 * raise each group's leader, then size. */
	QDockWidget *found[BUILTIN_DOCK_N] = {};
	for (int i = 0; i < BUILTIN_DOCK_N; i++) {
		found[i] = BuiltinDock(BUILTIN_DOCKS[i].id);
		if (!found[i]) {
			continue;
		}
		found[i]->setFloating(false);
		addDockWidget(BUILTIN_DOCKS[i].area, found[i]);
	}

	QHash<int, int> leader;
	for (int i = 0; i < BUILTIN_DOCK_N; i++) {
		if (!found[i] || !Tabifiable(BUILTIN_DOCKS[i])) {
			continue;
		}
		const int group = BUILTIN_DOCKS[i].group;
		if (leader.contains(group)) {
			tabifyDockWidget(found[leader.value(group)], found[i]);
		} else {
			leader.insert(group, i);
		}
	}

	for (int i = 0; i < BUILTIN_DOCK_N; i++) {
		if (found[i]) {
			found[i]->setVisible(BUILTIN_DOCKS[i].visibleByDefault);
		}
	}

	/* Qt makes the LAST dock added to a tab bar current, so without this a fresh layout opens on
	 * Stats rather than on the Mixer. Tested against visibleByDefault, not isVisible(): this runs
	 * from OBSInit, and every child of a window that has not been shown yet reports invisible —
	 * the same trap documented in QCiRigDocks::ApplyDefaultLayout() Pass 4. */
	for (auto it = leader.constBegin(); it != leader.constEnd(); ++it) {
		if (BUILTIN_DOCKS[it.value()].visibleByDefault) {
			found[it.value()]->raise();
		}
	}

	/* One tab bar means one width to divide, not three. Controls keeps its own slice because it
	 * is not in the group. */
	QList<QDockWidget *> bottomDocks{ui->mixerDock, controlsDock};
	resizeDocks(bottomDocks, {bottomDocksHeight, bottomDocksHeight}, Qt::Vertical);
	resizeDocks(bottomDocks, {cx * 60 / 100, cx * 16 / 100}, Qt::Horizontal);

	int sideDockWidth = std::min(width() * 30 / 100, 280);
	resizeDocks({ui->scenesDock}, {sideDockWidth}, Qt::Horizontal);

	/* THE RIG DOCKS ARE PART OF THIS FORK'S DEFAULT LAYOUT, so "Reset Docks" has to put them
	 * back rather than leave them where RESET_DOCKLIST above just threw them. That macro floats
	 * and then HIDES every extra dock, which is correct for a user's ad-hoc browser docks and
	 * exactly wrong for docks the fork ships: a fresh profile calls this function (OBSInit does
	 * it whenever there is no saved DockState), so without this line "usable out of the box"
	 * would mean five invisible docks and a menu the operator has to discover.
	 *
	 * It goes last on purpose: it re-homes, tabifies and re-shows, and every one of those has to
	 * happen after restoreState(startingDockLayout). */
	QCiRigDocks::ApplyDefaultLayout(this);

	activateWindow();
}

void OBSBasic::on_lockDocks_toggled(bool lock)
{
	QDockWidget::DockWidgetFeatures features =
		lock ? QDockWidget::NoDockWidgetFeatures
		     : (QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
			QDockWidget::DockWidgetFloatable);

	QDockWidget::DockWidgetFeatures mainFeatures = features;
	mainFeatures &= ~QDockWidget::QDockWidget::DockWidgetClosable;

	/* One pass over the table instead of six hand-written lines, and BEHAVIOUR-IDENTICAL to them:
	 * statsDock was the only one of the six passed `features` (i.e. keeping its close button),
	 * every other one got `mainFeatures`. That is now the `closable` column rather than the
	 * position of one line in a list of six. */
	for (int i = 0; i < BUILTIN_DOCK_N; i++) {
		QDockWidget *dock = BuiltinDock(BUILTIN_DOCKS[i].id);
		if (!dock) {
			continue;
		}
		dock->setFeatures(BUILTIN_DOCKS[i].closable ? features : mainFeatures);
	}

	for (int i = extraDocks.size() - 1; i >= 0; i--) {
		extraDocks[i]->setFeatures(features);
	}

	for (int i = extraCustomDocks.size() - 1; i >= 0; i--) {
		extraCustomDocks[i]->setFeatures(features);
	}

#ifdef BROWSER_AVAILABLE
	for (int i = extraBrowserDocks.size() - 1; i >= 0; i--) {
		extraBrowserDocks[i]->setFeatures(features);
	}
#endif
}

void OBSBasic::on_sideDocks_toggled(bool side)
{
	config_set_bool(App()->GetUserConfig(), "BasicWindow", "SideDocks", side);

	setDockCornersVertical(side);
}

void OBSBasic::AddDockWidget(QDockWidget *dock, Qt::DockWidgetArea area, bool extraBrowser)
{
	if (dock->objectName().isEmpty()) {
		return;
	}

	bool lock = ui->lockDocks->isChecked();
	QDockWidget::DockWidgetFeatures features =
		lock ? QDockWidget::NoDockWidgetFeatures
		     : (QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
			QDockWidget::DockWidgetFloatable);

	setupDockAction(dock);
	dock->setFeatures(features);
	addDockWidget(area, dock);

#ifdef BROWSER_AVAILABLE
	if (extraBrowser && extraBrowserMenuDocksSeparator.isNull()) {
		extraBrowserMenuDocksSeparator = ui->menuDocks->addSeparator();
	}

	if (!extraBrowser && !extraBrowserMenuDocksSeparator.isNull()) {
		ui->menuDocks->insertAction(extraBrowserMenuDocksSeparator, dock->toggleViewAction());
	} else {
		ui->menuDocks->addAction(dock->toggleViewAction());
	}

	if (extraBrowser) {
		return;
	}
#else
	UNUSED_PARAMETER(extraBrowser);

	ui->menuDocks->addAction(dock->toggleViewAction());
#endif

	extraDockNames.push_back(dock->objectName());
	extraDocks.push_back(std::shared_ptr<QDockWidget>(dock));
}

void OBSBasic::RemoveDockWidget(const QString &name)
{
	if (extraDockNames.contains(name)) {
		int idx = extraDockNames.indexOf(name);
		extraDockNames.removeAt(idx);
		extraDocks[idx].reset();
		extraDocks.removeAt(idx);
	} else if (extraCustomDockNames.contains(name)) {
		int idx = extraCustomDockNames.indexOf(name);
		extraCustomDockNames.removeAt(idx);
		removeDockWidget(extraCustomDocks[idx]);
		extraCustomDocks.removeAt(idx);
	}
}

bool OBSBasic::IsDockObjectNameUsed(const QString &name)
{
	/* From the table, not from six string literals repeated a fourth time. This check is the one
	 * whose omission is silent AND damaging: two docks sharing an objectName fuse under
	 * saveState/restoreState, so a builtin missing from this list can be shadowed by a plugin
	 * dock and the operator's saved layout quietly merges the two. */
	QStringList list;
	for (int i = 0; i < BUILTIN_DOCK_N; i++) {
		list << QString::fromUtf8(BUILTIN_DOCKS[i].objectName);
	}
	list << extraDockNames;
	list << extraCustomDockNames;

	return list.contains(name);
}

void OBSBasic::AddCustomDockWidget(QDockWidget *dock)
{
	// Prevent the object name from being changed
	connect(dock, &QObject::objectNameChanged, this, &OBSBasic::RepairCustomExtraDockName);

	bool lock = ui->lockDocks->isChecked();
	QDockWidget::DockWidgetFeatures features =
		lock ? QDockWidget::NoDockWidgetFeatures
		     : (QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
			QDockWidget::DockWidgetFloatable);

	dock->setFeatures(features);
	addDockWidget(Qt::RightDockWidgetArea, dock);

	extraCustomDockNames.push_back(dock->objectName());
	extraCustomDocks.push_back(dock);
}

void OBSBasic::setDockCornersVertical(bool vertical)
{
	if (vertical) {
		setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
		setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
		setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
		setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
	} else {
		setCorner(Qt::TopLeftCorner, Qt::TopDockWidgetArea);
		setCorner(Qt::TopRightCorner, Qt::TopDockWidgetArea);
		setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
		setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
	}
}

void OBSBasic::RepairCustomExtraDockName()
{
	QDockWidget *dock = reinterpret_cast<QDockWidget *>(sender());
	int idx = extraCustomDocks.indexOf(dock);
	QSignalBlocker block(dock);

	if (idx == -1) {
		blog(LOG_WARNING, "A custom dock got its object name changed");
		return;
	}

	blog(LOG_WARNING, "The custom dock '%s' got its object name restored", QT_TO_UTF8(extraCustomDockNames[idx]));

	dock->setObjectName(extraCustomDockNames[idx]);
}
