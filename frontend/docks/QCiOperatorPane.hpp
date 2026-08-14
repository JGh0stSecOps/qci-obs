/******************************************************************************
    QCi Studio — the operator section. ONE dock, not six.

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

#include <QHash>
#include <QString>
#include <QWidget>

class QButtonGroup;
class QLabel;
class QPushButton;
class QSpinBox;
class QStackedWidget;

namespace QCiRig {
struct Model;
}

/*
 * ══ THE OPERATOR SECTION ═══════════════════════════════════════════════════════════════════════
 *
 * The brief's central ask made structural: "a dedicated section of the qci-studio". ONE dock, not
 * six, and not a browser dock anywhere in it.
 *
 * ⚠️ WHY ONE DOCK AND NOT SIX, IN PIXELS. Six QDockWidgets cost six Qt title bars — this fork's own
 * measurement, recorded in QCiRigDocks.cpp, is ~62-66px each — plus a Qt tab bar over the five that
 * shared one. That is upwards of 400px of Qt chrome in a 422px column, and a Qt tab bar is the
 * single most recognisably-OBS object in the whole window. Collapsing to one pane with an internal
 * segmented pager removes every piece of it. This is the difference between "OBS with extra docks"
 * and an operator console, and it is the difference the operator has twice said was missing.
 *
 * ⚠️ THREE LAYERS, AND THE OUTER TWO CANNOT BE PAGED, SCROLLED OR TABBED AWAY.
 *
 *   qciOpPrivacy    PINNED  PRIVACY HOLD · the privacy readout · the vcam outcome ·
 *                           MASK VISION|BLOCK · BRB · MIRROR
 *   qciOpPages      34px    QUEUE / REACT / CHAT / TANK / AUDIO — checkable buttons, NOT a QTabBar
 *   qciOpStack      fills   the five panes, one at a time
 *   qciOpTransport  PINNED  GO LIVE|ON AIR · GO · ARM/CANCEL/PHASE · the command readout
 *
 * Privacy above the pager and transport below it is not a taste decision. A pager is a way to hide
 * things — which is what a tab bar is — so folding six docks into five pages would have moved
 * PRIVACY HOLD behind a page instead of behind a tab and changed nothing that matters. A panic
 * control at the top of a SCROLLING column satisfies none of "always visible, one click, never
 * behind a tab" the moment somebody scrolls down to read a queue, which is the ordinary use of this
 * pane. Not being tabified is necessary and is not sufficient.
 *
 * WHAT MOVED OUT, so nobody looks for it here: the old CONTROL pane's 2-up scene grid became the
 * scene rail (docks/QCiSceneRail.*), and its six vitals became the flight strip's chips
 * (widgets/QCiFlightStrip.*). Each of those facts now has exactly one home.
 */
class QCiOperatorPane : public QWidget {
	Q_OBJECT

public:
	explicit QCiOperatorPane(QWidget *parent = nullptr);

	/** Show a page by its verb — "queue", "react", "chat", "tank", "audio". An unknown name is
	 *  IGNORED rather than falling through to page 0: a command-line typo must not silently move the
	 *  operator to a different pane than the one they named. */
	void showPage(const QString &name);

signals:
	/** The HUB block's freshness, for the dock title bar's right-hand readout. THREE STATES, never
	 *  two: "never answered", "current" and "answered a while ago" are different rig states, and
	 *  collapsing the first and third is how a panel shows a calm face over an unreachable server. */
	void freshnessChanged(const QString &word, const QString &tone);

private slots:
	void onModelChanged();

private:
	QWidget *buildPrivacyBlock();
	QWidget *buildPager();
	QWidget *buildTransport();
	void renderModel(const QCiRig::Model &model);

	/* ── the pinned privacy block ─────────────────────────────────────────────────────────────── */
	QPushButton *m_panic = nullptr;
	QLabel *m_privacy = nullptr;
	QLabel *m_vcam = nullptr;
	QPushButton *m_maskVision = nullptr;
	QPushButton *m_maskBlock = nullptr;
	QPushButton *m_brb = nullptr;
	QPushButton *m_mirror = nullptr;
	QLabel *m_mirrorWhy = nullptr;

	/* ⚠️ THE HOLD IS WATCHED AFTER IT IS PRESSED, AND THAT IS NOT BELT AND BRACES.
	 *
	 * MEASURED BY READING THE SOURCE, NOT BY RUNNING IT: obs-assets/watchdog/watchdog.mjs check()
	 * runs every 80ms plus on eleven OBS events, and its branch order is faults→kill(); else
	 * held→restore() after four clean streaks; ELSE IF prog === HOLD →
	 * SetCurrentProgramScene(lastGoodProgram), logging "program forced back to the air-gapped
	 * wrapper". PRIVACY HOLD is itself in PROGRAM_OK, so a MANUAL hold on a healthy chain raises no
	 * fault, `held` stays false (grep for `held =` finds it assigned only inside kill() and
	 * restore()), and that third branch fires. The mask and the virtual-camera cut persist —
	 * restore() only runs while held — but the SCENE does not. There is no manual-hold latch
	 * anywhere in the rig.
	 *
	 * watchdog.log neither confirms nor refutes it: it contains zero "forced back" lines, but it
	 * also contains five successful connections against 152010 "ws connection closed" lines, so the
	 * branch has simply never been exercised. That is not evidence of safety.
	 *
	 * So this pane does the one thing it can do from inside the fork: after a hold it watches the
	 * program scene for a few seconds and SAYS SO, loudly and persistently, if the hold did not
	 * stick. A panic button that undoes itself silently is worse than one that does not exist. The
	 * real fix is a latch in watchdog.mjs and it is not this repository's to make. */
	qint64 m_holdWatchUntil = 0;
	QString m_holdScene;
	QLabel *m_holdAlarm = nullptr;

	/* ── the pager ────────────────────────────────────────────────────────────────────────────── */
	QButtonGroup *m_pageButtons = nullptr;
	QStackedWidget *m_stack = nullptr;
	QHash<QString, int> m_pageIndex;

	/* ── the pinned transport foot ────────────────────────────────────────────────────────────── */
	QPushButton *m_stream = nullptr;
	QPushButton *m_go = nullptr;
	QSpinBox *m_armMins = nullptr;
	QPushButton *m_arm = nullptr;
	QPushButton *m_cancel = nullptr;
	QLabel *m_phase = nullptr;
	QLabel *m_say = nullptr;

	QString m_freshness;
};
