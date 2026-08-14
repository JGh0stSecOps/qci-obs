/******************************************************************************
    QCi Studio — THE one client for the rig's operator surface.

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

#include "QCiRigModel.hpp"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class QWidget;

/*
 * ══ ONE CLIENT ═════════════════════════════════════════════════════════════════════════════════
 *
 * EVERY REQUEST THIS FORK MAKES TO THE RIG IS MADE HERE. No pane owns a QNetworkAccessManager, no
 * pane knows a route name, no pane holds a timer, and no pane keeps a field of its own that it
 * refreshes on a press. They take `const QCiRig::Model &` and return void.
 *
 * ⚠️ THE ALTERNATIVE IS NOT HYPOTHETICAL — IT IS WHAT THE 7" CONSOLE HAD TO BE RESCUED FROM.
 * events/control.html grew one poller per pane, and the file now carries the post-mortems: the
 * moderation poll and the OBS poll "overwrote each other AND the intent could win — meaning the
 * panel could assert a program scene that OBS had refused or that the privacy watchdog had already
 * overridden", and the level poll with no in-flight guard "fired 6.7 times a second into that hole
 * with no in-flight guard, so within about a second all SIX of the browser's per-host connections
 * to the events server were parked on requests that would never finish... The one moment the
 * operator most needs this panel is the moment OBS is down, and that is exactly when it wedged
 * itself." (The address is elided from that quote on purpose: qci-rig/lib/studiodocks.test.mjs
 * bans it from every file but the one that owns it, prose included, because help text and comments
 * are where a stale address survives longest — they never throw.)
 *
 * Both of those are properties of HOW MANY THINGS FETCH, not of what they fetch. So this class is
 * the answer to both: one tick, one in-flight request per surface, one abort budget, one backoff,
 * one model, one signal.
 *
 * ⚠️ FIVE SURFACES, AND THE SPLIT IS A SECURITY BOUNDARY. See the long note in QCiRigModel.hpp:
 * qci-rig's lib/hub.test.mjs forbids moderation state, donor names, dollar amounts and chat lines
 * from ever appearing in the /hub document, because /hub is the route exposed to a LAN device whose
 * flash has already been dumped. This fork is on loopback and may read the rest; it does NOT get to
 * "simplify" by asking the rig to widen /hub.
 *
 * ⚠️ THE SAFETY BLOCK IS ONE INSTANT AND THE OTHERS ARE NOT, and the panel says which is which.
 * scene / mask / held / live / vitals all arrive in a single /hub reply, which is the property
 * lib/hub.mjs spends its header arguing for. The moderation, reaction, chat and audio blocks are
 * separate replies on separate cadences, each with its own age and its own fault, and a pane whose
 * block has gone stale prints that rather than drawing an old list as current.
 */
class QCiRigClient : public QObject {
	Q_OBJECT

public:
	/* The surfaces this fork reads. The order is the poll order within a tick. */
	enum Surface {
		SurfaceHub = 0,  /* GET /hub           — the safety block, every tick, ALWAYS */
		SurfaceQueue,    /* GET /queue         — moderation */
		SurfaceReactions,/* GET /reactions     — the operator's play queue + transport */
		SurfaceChat,     /* GET /chat          — the chat embed URL, read back out of OBS */
		SurfaceAudio,    /* GET /audio-routing — where the audio is going */
		SurfaceObs,      /* GET /obs           — ONE bit: the mirror's three-state answer */
		SurfaceCount,
	};

	/* One process, one client. Created on first use and parented to the app so it outlives every
	   dock — a dock the operator closes must not take the panic control's state feed with it. */
	static QCiRigClient *Get();

	const QCiRig::Model &model() const { return m_model; }

	/** The clock every `Freshness::at` in the model is stamped from. MONOTONIC and session-local,
	 *  never a wall clock: a panel that subtracts two epochs across a clock adjustment reports an
	 *  age that is wrong in the one direction that matters, and the whole point of an age here is
	 *  to answer "is what I am looking at still true". */
	qint64 nowMs() const;

	/* ── who is looking ───────────────────────────────────────────────────────────────────────
	 * A poll for a surface nobody can see is pure cost, and on this rig it is cost paid against
	 * the same loopback server that is answering the privacy mask. Panes declare which surface
	 * they draw; the client polls a surface only while at least one of its panes is visible.
	 *
	 * SurfaceHub is exempt and always polled: the privacy state has to be current the instant the
	 * operator looks, and the panic pane is never hideable anyway. */
	void wantSurface(Surface surface, QWidget *pane);

	/* Re-read everything now, whatever the tick schedule says. Used after a write, so the panel
	   shows what the SERVER did rather than what was pressed. */
	void refresh();

	/* Start/stop the tick. Load() starts it; nothing else needs to. */
	void start();

	/* ── the write verbs ──────────────────────────────────────────────────────────────────────
	 * ⚠️ TWO TRANSPORTS, AND WHICH ONE A VERB USES IS DECIDED BY THE RIG, NOT BY CONVENIENCE.
	 *
	 * The first six go through POST /hub/action, which is the CLOSED verb list qci-rig's
	 * lib/hub.mjs gates for the untrusted LAN panels — same vocabulary, same gate, same refusal
	 * shape, so this fork and the ESP32 boards cannot ask for different things or be told no in
	 * different words.
	 *
	 * The rest have no verb there ON PURPOSE: approving a viewer's message onto a live broadcast
	 * and driving the reaction player are not powers that belong to a device anyone can pick up
	 * off the desk. They go to the loopback-only routes, which is a capability this fork has and
	 * the panels do not. Do not "unify" them by adding verbs to HUB_ACTIONS. */
	void actHold();                                 /* PANIC. scene + mask + cut, one decision. */
	void actMask(const QString &mode);              /* "vision" | "block" — never a toggle */
	void actBrb(bool on);                           /* named direction, never a toggle */
	void actMirror(bool on);                        /* named direction, never a toggle */
	void actScene(const QString &realSceneName);    /* the REAL name, never the printed label */
	void actStream(bool on);                        /* named direction, never a toggle */
	void actEmote(const QString &name);
	void actGo();                                   /* hand off from the boot hold */

	void actApprove(qint64 id);
	void actReject(qint64 id);
	void actApproveAll();
	void actAutoToggle();
	/* ⚠️ NAMED FOR WHAT IT DOES, NOT FOR WHAT IT IS FOR. GET /event injects a REAL queue item that
	   counts toward the goal (isMoney → state.goal.current += amount). The button that calls this
	   says so on its face; a verb called actTest() with a tooltip saying "test" would not. */
	void actInjectPaidTestItem();
	void actGoal(double current, double target, const QString &label);
	void actGoalClear();

	void actShare(const QString &url);
	void actShareClear();

	/* The scripted start. Zero minutes means START NOW and must survive the trip — see the note in
	   the .cpp; the rig parses `q.min === undefined ? NaN : Number(q.min)` precisely because
	   `Number(x)||DEFAULT` swallowed a zero and turned it into a 15-minute countdown. */
	void actArm(int minutes);
	void actArmCancel();

	void actReactionAdd(const QString &url);
	void actReactionPlay(const QString &id);
	void actReactionRemove(const QString &id);
	void actReactionMove(const QString &id, int to);
	void actReactionPrev();
	void actReactionNext();
	void actReactionPause();
	void actReactionResume();
	void actReactionSeekBy(double seconds);
	void actReactionRateStep();

	void actChannelMute(const QString &input, bool muted);

signals:
	/* ONE SIGNAL. Panes connect, then read model(). The model is not carried in the signal on
	   purpose: a pane that received a copy could hold it, and a held copy is a second idea of the
	   rig's shape, which is the whole thing this class exists to prevent. */
	void changed();

	/* The operator-facing one-line readout: what was asked, and — far more importantly — what was
	   REFUSED and why, in the server's own words. control.html's rule, kept: "a refusal here is not
	   an error to swallow... that sentence is the whole point of having asked." */
	void say(const QString &text);

	/* ⚠️ THE PANIC'S THIRD STEP REPORTS SEPARATELY, BECAUSE IT IS THE ONE OUTCOME THE OPERATOR
	 * CANNOT INFER FROM ANYTHING ELSE ON THE GLASS.
	 *
	 * The virtual camera is PINNED in this collection (type2:1 SceneOutput, scene "PANELS (Delayed
	 * Output)"), so it renders its pinned target regardless of program and a scene switch does not
	 * take it off air at all. Only StopVirtualCam does. hubAction's hold awaits cutVirtualCam() on a
	 * 2000ms budget and answers with one of three words — "stopped" | "idle" | "unreachable" — and
	 * those are three different rig states: the cut happened, there was nothing to cut, and NOBODY
	 * KNOWS. Flattening them into the one-line say() readout would make the third look like the
	 * second. `scene` is the hold scene the rig actually switched to, carried so the caller can
	 * check that it STAYED (see the watchdog note in QCiRigPanes.cpp). */
	void holdAnswered(bool ok, const QString &vcam, const QString &scene);

private:
	explicit QCiRigClient(QObject *parent);

	void tick();
	void poll(Surface surface);
	void finish(Surface surface, QNetworkReply *reply);
	bool surfaceWanted(Surface surface) const;

	/* GET a route and hand the parsed body to the surface. */
	void get(Surface surface, const QString &route);
	/* Fire-and-forget write; reports a refusal through say() and then refreshes.
	 *
	 * `onReply` is handed the parsed body and whether this client was willing to call it a success —
	 * it exists for actHold() and nothing else. It is NOT a general "and then do this": every other
	 * verb renders from the next poll, because "the server is the authority, always" and a caller
	 * that acts on a write's own reply is a caller keeping a second idea of the rig's state. The
	 * hold is the exception because its vcam word is not published anywhere a poll can reach. */
	void write(const QString &route, bool post, const QString &okText,
		   std::function<void(bool ok, const QJsonObject &body)> onReply = nullptr);

	QNetworkAccessManager *m_net = nullptr;
	QTimer *m_tick = nullptr;
	QElapsedTimer m_clock;

	QCiRig::Model m_model;

	/* Per surface: the in-flight reply (one, never two), how many consecutive failures, and when
	   the backoff allows the next attempt. Straight from the console's measured wedge. */
	struct SurfaceState {
		QPointer<QNetworkReply> inFlight;
		int fails = 0;
		qint64 nextAt = 0;
		int ticksSincePoll = 0;
		QVector<QPointer<QWidget>> panes;
	};
	SurfaceState m_surface[SurfaceCount];

	qint64 m_ticks = 0;
	/* Set by refresh(): the next tick polls every wanted surface regardless of its divisor. */
	bool m_forceAll = false;
};
