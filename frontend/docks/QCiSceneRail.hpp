/******************************************************************************
    QCi Studio — the scene rail. A switcher, not a file browser.

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

#include <QList>
#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace QCiRig {
struct Model;
}

/*
 * ══ THE SCENE RAIL ═════════════════════════════════════════════════════════════════════════════
 *
 * Scene switching is the highest-frequency live action on this rig, and stock OBS renders it as a
 * tree row about 20px tall with a seven-button editing toolbar underneath it. That is the right
 * shape for BUILDING a collection and the wrong shape for switching one mid-sentence: a solo
 * operator who is talking needs a target they can hit without looking. So this is a vertical column
 * of 56px program-safe buttons — what a switcher looks like — with NO add / remove / duplicate /
 * rename toolbar at all. Those are construction verbs and they stay in OBS's own scenes dock.
 *
 * ⚠️ IT IS BUILT FROM GET /hub's `scenes` AND FROM NOTHING ELSE — THERE IS NO COMPILED-IN FALLBACK
 * AND THERE MUST NEVER BE ONE. The rig publishes the intersection of what config's PROGRAM_OK
 * permits with what OBS actually has, so an empty list means one of those is unreadable, and the
 * correct rendering of that is a sentence rather than buttons. The console shipped one hand-copied
 * allowlist (SCENE_FALLBACK) and it went stale exactly once, which was enough: it offered a RETIRED
 * wrapper — the one that trips the privacy kill — while omitting the maintained one and INCOMING
 * RAID. A rail that offers a scene the watchdog will kill 80ms later is worse than an empty rail.
 *
 * ⚠️ THE BUTTON PRINTS `l` AND SENDS `n`. Every route that carries the pair restates this, because
 * the label has had " (Delayed Output)" stripped off it and a client that POSTs the pretty string
 * asks for a scene that does not exist — which OBS answers by switching nothing, silently. The real
 * name is held in the button's own data and is never re-derived from what is drawn on it.
 */
class QCiSceneRail : public QWidget {
	Q_OBJECT

public:
	explicit QCiSceneRail(QWidget *parent = nullptr);

	/** Switch to the n-th row, 0-based. Wired to keys 1-9 as menu actions so the application
	 *  ADVERTISES its verbs rather than hiding them in a help modal. Does nothing when there is no
	 *  such row: a key pressed into a short rail must not switch the last scene in the list. */
	void activateRow(int index);

signals:
	/** The rail's own freshness, for the dock title bar's right-hand readout. THREE STATES, never
	 *  two — "never answered", "current" and "answered a while ago" are different rig states, and
	 *  collapsing the first and third is how a panel shows an empty rail that is really a dead
	 *  server. */
	void freshnessChanged(const QString &word, const QString &tone);

private slots:
	void onModelChanged();

private:
	void rebuild(const QCiRig::Model &model);

	QVBoxLayout *m_column = nullptr;
	QLabel *m_empty = nullptr;
	QList<QPushButton *> m_rows;
	QString m_sig;
	QString m_freshness;
};
