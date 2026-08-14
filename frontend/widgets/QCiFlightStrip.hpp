/******************************************************************************
    QCi Studio — the flight strip. The band that is in no OBS build.

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
#include <QWidget>

class QLabel;
class QPushButton;
class QTimer;

namespace QCiRig {
struct Model;
}

/*
 * ══ THE FLIGHT STRIP ═══════════════════════════════════════════════════════════════════════════
 *
 * A 48px band across the whole top of the window, above BOTH dock columns, present in RUN and in
 * BUILD, and closable by nothing.
 *
 * WHY IT IS A QToolBar AND NOT A DOCK. Toolbar areas in a QMainWindow sit OUTSIDE the four dock
 * widget areas, so a toolbar spans the full window width while a top dock would be squeezed between
 * the left and right columns. It is also the only container in QMainWindow that cannot be dragged
 * into a tab bar, which is the failure mode QCiRigDocks spends two hundred lines defending against.
 * The toolbar is set non-movable, non-floatable, and its context menu is suppressed so that OBS's
 * own right-click-a-toolbar-to-hide-it cannot reach it.
 *
 * WHAT IT ANSWERS, and this is why it exists: a live streamer's question at any instant is "am I on
 * air, on what scene, with the mask how, and is the chain healthy". Stock OBS scatters those five
 * answers across a status bar, a dock, a Stats window and nowhere. Here they are one band that is
 * always on the glass, so the operator never NAVIGATES to check state.
 *
 * ⚠️ EVERY VALUE IS THE SERVER'S OR LIBOBS'S. Nothing here is remembered, accumulated or derived
 * from a previous frame — in particular the uptime clock is /hub's `sinceMs`, a DURATION the server
 * computed, and never a start time this process wrote down. An app that restarts mid-stream would
 * otherwise show a clock that is confidently wrong, and a confidently wrong clock is worse than no
 * clock on a rig whose whole rule is "never show green over a dead link".
 */
class QCiFlightStrip : public QWidget {
	Q_OBJECT

public:
	explicit QCiFlightStrip(QWidget *parent = nullptr);

	/** Reflect the window's mode in the RUN|BUILD segment WITHOUT emitting a request. Called by the
	 *  mode machinery, which is the authority; the segment is a view of it, not a second copy. */
	void setBuildMode(bool build);

signals:
	/** The operator pressed RUN or BUILD. OBSBasic decides whether that happens; this only asks. */
	void modeRequested(bool build);

private slots:
	void onModelChanged();

private:
	QWidget *makeChip(const char *objectName, const QString &legend, QLabel **valueOut, int width);
	QLabel *makeLamp(const char *objectName, const QString &legend);
	void setLamp(QLabel *lamp, const QString &tone);
	void renderModel(const QCiRig::Model &model);

	/* ── the live block ─────────────────────────────────────────────────────────────────────── */
	QWidget *m_air = nullptr;
	QLabel *m_airLamp = nullptr;
	QLabel *m_airText = nullptr;
	QLabel *m_recDot = nullptr;
	QLabel *m_clock = nullptr;

	/* ── what is on program, and how the face is ────────────────────────────────────────────── */
	QLabel *m_scene = nullptr;
	QLabel *m_mask = nullptr;
	QLabel *m_watchdog = nullptr;

	/* ── vitals ─────────────────────────────────────────────────────────────────────────────── */
	QLabel *m_br = nullptr;
	QLabel *m_fps = nullptr;
	QLabel *m_drop = nullptr;
	QLabel *m_lag = nullptr;
	QLabel *m_cpu = nullptr;

	/* ── moderation and money ───────────────────────────────────────────────────────────────── */
	QLabel *m_q = nullptr;
	QLabel *m_a = nullptr;
	QLabel *m_goal = nullptr;
	QWidget *m_vod = nullptr;

	/* ── health ─────────────────────────────────────────────────────────────────────────────── */
	QLabel *m_lampEvt = nullptr;
	QLabel *m_lampObs = nullptr;

	QPushButton *m_run = nullptr;
	QPushButton *m_build = nullptr;

	/* THE ONE ANIMATION IN THE CHROME. 850ms on / 850ms off = 0.59Hz, on a 10px lamp and nothing
	   else. The 168x40 amber field beside it does NOT blink: a large-area strobe is the actual
	   photosensitivity hazard, and the rig's whole motion language is sub-3Hz and stepped. */
	QTimer *m_blink = nullptr;
	bool m_blinkOn = false;
	bool m_live = false;
};
