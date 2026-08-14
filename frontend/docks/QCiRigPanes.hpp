/******************************************************************************
    QCi Studio — the native operator panel. Real Qt widgets, one model.

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

#include "QCiRigClient.hpp"

#include <QColor>
#include <QHash>
#include <QString>
#include <QWidget>

class QButtonGroup;
class QDoubleSpinBox;
class QFrame;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QStackedWidget;
class QVBoxLayout;

/*
 * ══ THE PANES ══════════════════════════════════════════════════════════════════════════════════
 *
 * This is the "dedicated section of qci-studio" the operator asked for, and the reason it is not
 * browser docks any more is in their own words: "Now that we forked we no longer need browser
 * docks. Build it in first class in the source code."
 *
 * WHAT A PANE IS ALLOWED TO DO: read `const QCiRig::Model &` and set widget state from it, and call
 * a verb on QCiRigClient. That is the whole contract.
 *
 * WHAT A PANE MAY NOT DO, and each of these is a bug the 7" console had to be rescued from:
 *   - own a network object or a poll timer          (six connections wedged on a route OBS hung)
 *   - keep a field it refreshes on a press          (the panel asserted a scene OBS had refused)
 *   - derive a fact the server already publishes    (a second copy of the capability table)
 *   - render optimistically from what was pressed   ("the server is the authority, always")
 *
 * ⚠️ RENDERING IS IDEMPOTENT AND CHEAP, LIST REBUILDS ARE NOT. Every pane is re-rendered on every
 * model change — up to once a second — so scalar labels are simply assigned, while the three lists
 * (moderation, reactions, audio) rebuild only when a SIGNATURE of their contents changes. That is
 * the same defence control.html uses (`qsig`/`asig`/`rxSig`) and it exists for a concrete reason
 * beyond cost: rebuilding a list under the operator's cursor moves the APPROVE button out from
 * under a press that is already happening.
 */

/* ── THE GOAL METER ─────────────────────────────────────────────────────────────────────────────
 * A row of DISCRETE CELLS, not a QProgressBar, and the cell count is the point: the rig's whole
 * visual language is stepped — the ESP32 panels, the tank's 32x16 matrix, the console's 40-cell goal
 * bar and 18-cell audio meters — and a smooth gradient bar in the middle of it reads as a control
 * borrowed from somewhere else.
 *
 * ⚠️ THE COLOURS ARE Q_PROPERTYs SO THE THEME STILL OWNS THEM. Nothing in QCiRigPanes.cpp names a
 * hex; a custom-painted widget would be the one place that rule could quietly break, so the four
 * colours are set from data/themes/QCi.obt with `qproperty-`. That keeps this widget inside the
 * palette gate qci-rig/lib/studiotheme.test.mjs enforces over that file — and the filled cells are
 * the one place in the application besides a donation figure where pink is legal, because a goal
 * meter is a rendering of money.
 */
class QCiGoalMeter : public QWidget {
	Q_OBJECT
	Q_PROPERTY(QColor cellUnlit MEMBER m_unlit)
	Q_PROPERTY(QColor cellFilled MEMBER m_filled)
	Q_PROPERTY(QColor cellOver MEMBER m_over)

public:
	explicit QCiGoalMeter(QWidget *parent = nullptr);

	/** Absent means "the rig has not published a goal", which draws as an empty rail and never as
	 *  zero-of-something: a meter reading 0/$20 is a claim that nobody has given anything. */
	void setGoal(const QCiRig::Maybe<QCiRig::Goal> &goal);

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	QCiRig::Maybe<QCiRig::Goal> m_goal;
	QColor m_unlit;
	QColor m_filled;
	QColor m_over;
};

/** Shared scaffolding: a titled pane that re-renders when the one client says something moved. */
class QCiRigPane : public QWidget {
	Q_OBJECT

public:
	QCiRigPane(QCiRigClient::Surface surface, const QString &title, QWidget *parent = nullptr);

	/** ⚠️ DROP THE PANE'S OWN NAME, KEEP ITS FRESHNESS. Used when the pane is a PAGE inside the one
	 *  operator dock rather than a dock of its own: the segmented pager above it already says which
	 *  page this is, and a second copy of that word costs a row of a column that has none to spare.
	 *
	 *  THE STATUS READOUT STAYS, and that is not an oversight. QCiRigModel.hpp's header is emphatic
	 *  that the blocks are NOT one instant — "the moderation queue, the reaction list, the chat
	 *  embed and the audio routing ... are separate replies on separate timers, so each block
	 *  carries its own `at` and its own `fault`". The operator dock's head shows the HUB's freshness,
	 *  which says nothing about whether this page's own block has gone stale. Two blocks, two
	 *  readouts. */
	void setNameless();

protected:
	/** Called on every model change. Implementations read the model and assign; they do not fetch,
	 *  do not cache and do not decide anything the server has already decided. */
	virtual void renderModel(const QCiRig::Model &model) = 0;

	QCiRigClient *rig() const { return QCiRigClient::Get(); }

	/** The pane's body layout — implementations add their rows to this. It sits inside a scroll
	 *  area, because a dock column is narrow and the console's own rule is that the dock "is the
	 *  only surface allowed to scroll". */
	QVBoxLayout *body() const { return m_body; }

	/** ⚠️ THE LAYER THAT CANNOT SCROLL AWAY. Anything added here sits above the scroll area and is
	 *  on the glass whatever the operator has scrolled to.
	 *
	 *  This exists for exactly one widget: PRIVACY HOLD. The brief is "always visible, one click,
	 *  never behind a tab", and a panic control at the top of a scrolling column satisfies none of
	 *  that the moment somebody scrolls down to read a queue — which is the ordinary use of the
	 *  pane. Not being tabified is necessary and is not sufficient. */
	QVBoxLayout *pinned() const { return m_pinned; }

	/** The small readout on the right of the pane's title row. Used for the block's own state
	 *  ("QUEUED +3", "STALE 42S AGO"), never for a value that belongs in the body. */
	void setStatus(const QString &text, const QString &tone = QString());

	/** Hand the pinned layer the pane's whole height and take the scroll area out of the picture.
	 *
	 *  For panes whose body is ONE widget that must fill — today that is CHAT, whose embed is a
	 *  native window that has no business inside a scroll viewport (see the note in the .cpp). It is
	 *  a method rather than a constructor flag so the pinned/scrolled split stays the default and a
	 *  pane has to say out loud that it is opting out of it. */
	void pinBodyFullBleed();

	/** Draw the block's freshness as words. Returns true when the block is CURRENT; false means the
	 *  pane is showing the last thing the rig said and should not pretend otherwise.
	 *
	 *  ⚠️ THE THREE STATES ARE NOT TWO. "never answered", "answered and current" and "answered a
	 *  while ago" are different rig states and collapsing the first and third is how a panel shows
	 *  an empty queue that is actually an unreachable server. */
	bool applyFreshness(const QCiRig::Freshness &fresh);

private slots:
	void onChanged();

private:
	QWidget *m_header = nullptr;
	QLabel *m_title = nullptr;
	QFrame *m_rule = nullptr;
	QLabel *m_status = nullptr;
	QVBoxLayout *m_body = nullptr;
	QVBoxLayout *m_pinned = nullptr;
	QVBoxLayout *m_outer = nullptr;
	QScrollArea *m_scroll = nullptr;
};

/* THE SCENE RAIL AND THE PRIVACY/TRANSPORT BLOCKS ARE NOT IN THIS FILE. The old CONTROL pane held
   the panic button, the mask segment, a 2-up scene grid, the transport and six vitals; every one of
   those has moved to the surface that owns it — docks/QCiOperatorPane.* (privacy + transport, pinned
   above and below the pager rather than scrolling with a body) and docks/QCiSceneRail.* (a 56px
   switcher column). What is left in this file is the five PAGES. */

/* ── QUEUE — moderation. Loopback only; see the boundary note in QCiRigModel.hpp. ─────────────── */
class QCiRigQueuePane : public QCiRigPane {
	Q_OBJECT

public:
	explicit QCiRigQueuePane(QWidget *parent = nullptr);

protected:
	void renderModel(const QCiRig::Model &model) override;

private:
	QPushButton *m_auto = nullptr;
	QPushButton *m_approveAll = nullptr;
	QPushButton *m_inject = nullptr;
	QLabel *m_pendingCount = nullptr;
	QLabel *m_approvedCount = nullptr;
	QWidget *m_pendingBox = nullptr;
	QVBoxLayout *m_pendingList = nullptr;
	QWidget *m_approvedBox = nullptr;
	QVBoxLayout *m_approvedList = nullptr;
	QString m_pendingSig;
	QString m_approvedSig;

	/* ── the goal ──────────────────────────────────────────────────────────────────────────────
	 * ⚠️ THE RECONCILE GUARD IS "EDITED SINCE PUBLISHED", NOT "FOCUSED", AND THE DIFFERENCE IS A
	 * WRONG NUMBER ON STREAM. control.html hit this on the SHARE box first and wrote it down:
	 * mousedown on the button moves focus to the button BEFORE the click handler runs — 60-150ms
	 * against an 800ms poll — so a focus-based guard lets the poll overwrite what was typed, and the
	 * handler then publishes the OLD value with every readout agreeing and nothing erroring
	 * anywhere. So: a dirty flag set on textEdited/valueChanged-by-the-user, cleared only on a
	 * SUCCESSFUL write, and the reconciler skips a dirty field. Same class of bug, same fix, on the
	 * SHARE box in the TANK page. */
	QDoubleSpinBox *m_goalCurrent = nullptr;
	QDoubleSpinBox *m_goalTarget = nullptr;
	QLineEdit *m_goalLabel = nullptr;
	QPushButton *m_goalSet = nullptr;
	QPushButton *m_goalClear = nullptr;
	QCiGoalMeter *m_goalMeter = nullptr;
	QLabel *m_goalFigure = nullptr;
	bool m_goalDirty = false;
	/* What was last SENT, so the dirty flag can be cleared on proof rather than on a press. */
	double m_goalSentCurrent = 0.0;
	double m_goalSentTarget = 0.0;
};

/* ── REACTIONS — the operator's play queue, and a real transport over it ───────────────────────
 * NOT the moderation queue and never merged with it: that one holds viewer-submitted events
 * awaiting approval, this one holds links the operator chose. */
class QCiRigReactionsPane : public QCiRigPane {
	Q_OBJECT

public:
	explicit QCiRigReactionsPane(QWidget *parent = nullptr);

protected:
	void renderModel(const QCiRig::Model &model) override;

private:
	void submitUrl();

	QLineEdit *m_url = nullptr;
	QPushButton *m_add = nullptr;
	QPushButton *m_prev = nullptr;
	QPushButton *m_next = nullptr;
	QWidget *m_transport = nullptr;
	QPushButton *m_play = nullptr;
	QPushButton *m_pause = nullptr;
	QPushButton *m_back10 = nullptr;
	QPushButton *m_fwd10 = nullptr;
	QPushButton *m_rate = nullptr;
	QLabel *m_reason = nullptr;
	QWidget *m_listBox = nullptr;
	QVBoxLayout *m_list = nullptr;
	QString m_listSig;
	/* What was last handed to the rig, and how long the list was when it was handed over. The draft
	   box empties only when the list has actually GROWN since — proof the rig took the link, rather
	   than an assumption that it did. See submitUrl(). */
	QString m_submitted;
	int m_countAtSubmit = -1;
};

/* ── CHAT ───────────────────────────────────────────────────────────────────────────────────────
 * THE ONE SANCTIONED EXTERNAL VIEW IN THIS PANEL, and it is somebody else's page. There is no
 * native protocol for a live-chat embed to render; the rig does not carry chat MESSAGES anywhere,
 * it carries the URL of the embed the STREAM is already showing, read back out of OBS by
 * GET /chat rather than configured anywhere. So this pane hosts a web view when the fork has one,
 * and prints the server's own reason word when it does not.
 *
 * ⚠️ THE VIEW IS RELOADED ONLY WHEN THE URL ACTUALLY CHANGES. Reloading on every poll restarts
 * somebody else's player, drops their session and makes chat unreadable — the pane is polled, the
 * embed is not. */
class QCiRigChatPane : public QCiRigPane {
	Q_OBJECT

public:
	explicit QCiRigChatPane(QWidget *parent = nullptr);

protected:
	void renderModel(const QCiRig::Model &model) override;

private:
	QLabel *m_placeholder = nullptr;
	QWidget *m_view = nullptr;
	QString m_loadedUrl;
};

/* ── TANK — QC's face and the emote roster ──────────────────────────────────────────────────────
 * The roster is embody's own, relayed through /hub, and is NEVER a list this fork keeps: correcting
 * a compiled-in roster costs a rebuild, and the rig has spent real effort deleting every second
 * copy of it. Absent roster means no buttons and one honest sentence, not a guess. */
class QCiRigTankPane : public QCiRigPane {
	Q_OBJECT

public:
	explicit QCiRigTankPane(QWidget *parent = nullptr);

protected:
	void renderModel(const QCiRig::Model &model) override;

private:
	QLabel *m_face = nullptr;
	QWidget *m_rosterBox = nullptr;
	QGridLayout *m_roster = nullptr;
	QString m_rosterSig;

	/* ── SHARE — THE ONLY COPY IN THE APPLICATION ──────────────────────────────────────────────
	 * The console has TWO share boxes (CONTROL and TANK) reconciling from one server state, and the
	 * only reason they cannot disagree is that neither keeps state of its own. One box removes the
	 * question. It lives here rather than in the privacy block because putting a link on stream is
	 * not a privacy action and the pinned layer is spent.
	 *
	 * Dirty flag, exactly as the goal fields — see the note there; this is the box the bug was
	 * FOUND on. */
	QLineEdit *m_share = nullptr;
	QPushButton *m_shareLoad = nullptr;
	QPushButton *m_shareClear = nullptr;
	QLabel *m_shareState = nullptr;
	bool m_shareDirty = false;
	QString m_shareSent;
};

/* ── AUDIO — where the audio is going, and where it is coming from ─────────────────────────────
 * ⚠️ THIS PANE DELIBERATELY DRAWS NO LEVEL METERS. The fork IS OBS: it has obs_volmeter and a mixer
 * dock that paints real meters off the audio thread. Reproducing those from a 1 Hz HTTP poll would
 * be strictly worse than what the application already shows two docks away. What the mixer CANNOT
 * show is the thing the operator asked for — what is going where, off what device, at what added
 * latency, and whether the routing is legal — and that is what this pane is for. */
class QCiRigAudioPane : public QCiRigPane {
	Q_OBJECT

public:
	explicit QCiRigAudioPane(QWidget *parent = nullptr);

protected:
	void renderModel(const QCiRig::Model &model) override;

private:
	QLabel *m_monitor = nullptr;
	QLabel *m_latency = nullptr;
	QLabel *m_violations = nullptr;
	QLabel *m_taps = nullptr;
	QWidget *m_channelBox = nullptr;
	QVBoxLayout *m_channels = nullptr;
	QString m_channelSig;
};
