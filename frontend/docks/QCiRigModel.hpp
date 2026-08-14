/******************************************************************************
    QCi Studio — the rig's state, as ONE model.

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

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

/*
 * ══ THE MODEL ══════════════════════════════════════════════════════════════════════════════════
 *
 * ONE MODEL, THREE RENDERERS. The rig's operator surface is drawn in three places now — the 7"
 * browser console (qci-rig/events/control.html), the AITRIP ESP32 panels, and this fork — and the
 * way three renderers stop agreeing is each one keeping its own idea of the rig's shape. So this
 * header is the fork's ONLY description of that shape, every widget in the panel renders from a
 * `Model` and nothing else, and exactly one object (QCiRigClient) ever fills one in.
 *
 * ⚠️ OMIT, NEVER DEFAULT — AND THAT IS WHY EVERY SCALAR HERE IS AN std::optional.
 *
 * This is not C++ taste. It is the rule qci-rig/lib/hub.mjs is built around, stated in its own
 * header: "EVERY OPTIONAL KEY IS OMITTED, NOT DEFAULTED... a wrong host that answers is
 * indistinguishable from a live tank. A defaulted `mask` draws a privacy badge over an unmasked
 * camera. A defaulted `scene` names a scene OBS is not on." The rig went to the trouble of DELETING
 * keys from the wire so that absence would survive the trip; a parser that lands them in `QString
 * mask;` and `bool live = false;` throws that away at the last possible moment, in the client that
 * is running on the operator's own machine beside their face.
 *
 * So: `std::optional<bool> live` has three states and the panel must render all three — on air,
 * off air, and NOBODY HAS ASKED OBS ANYTHING SINCE IT DIED. The last one is not `false`.
 *
 * ⚠️ ONE INSTANT PER BLOCK, AND THE BLOCKS ARE NOT THE SAME INSTANT.
 *
 * lib/hub.mjs argues at length for why the safety-critical facts arrive together: "On a screen
 * whose whole job is to tell an operator whether their face is currently on a live broadcast, a
 * composite of four instants is not a state — it is four states drawn on top of each other." That
 * argument is honoured exactly: `hub` is filled from ONE reply, so scene / mask / held / live can
 * never disagree with each other about when they were true.
 *
 * It does NOT extend to the moderation queue, the reaction list, the chat embed or the audio
 * routing, and it must not be pretended otherwise — those are separate replies on separate timers,
 * so each block carries its own `at` and its own `fault`, and a pane whose block has gone stale
 * says so instead of drawing an old list as though it were current.
 *
 * ⚠️ WHY THERE IS MORE THAN ONE REPLY AT ALL — IT IS A SECURITY BOUNDARY, NOT AN OVERSIGHT.
 *
 * The obvious simplification is to widen GET /hub until it carries everything and poll one route.
 * The rig forbids it, in a test, with the reason: lib/hub.test.mjs asserts that the strings
 * "queue", "pending", "approved", "goal", "chat", "amount", "who", "text", "donor" and "message"
 * appear NOWHERE in the hub document — "moderation state has reached the panel" is the failure
 * message. /hub is the one route the operator is told to expose through a LAN proxy so the ESP32
 * boards can reach it, and those boards have no secure boot and a flash that has already been
 * dumped. Donor names, dollar amounts and chat lines have no business on that wire.
 *
 * This fork is on loopback, inside the operator's own OBS, so it may read the routes the LAN device
 * may not. The split below IS that boundary, drawn in the same place the rig draws it.
 */
namespace QCiRig {

/* Every optional in this file is spelled through this alias so that a grep for `Maybe<` finds every
   place the rig chose absence over a default, and so that a future move off std::optional is one
   line rather than sixty. */
template<typename T> using Maybe = std::optional<T>;

/* A scene, exactly as GET /hub publishes the pair: `n` is the REAL name and is what must be sent
   back, `l` is for printing. Both, always, and never one derived from the other — the rig restates
   this contract on every route that carries it because the label drops the " (Delayed Output)"
   suffix and a client that POSTs the pretty string switches nothing. */
struct Scene {
	QString name;
	QString label;
};

/* One viewer event awaiting moderation. GET /queue. */
struct QueueItem {
	qint64 id = 0;
	QString who;
	Maybe<double> amount;
	QString currency;
	QString platform;
	QString kind;
	QString text;
	bool tts = false;
};

/* The donation goal, exactly as GET /queue publishes it. Kept as a struct rather than three loose
   optionals because the three move together and a target with no current is not a goal. */
struct Goal {
	double current = 0.0;
	double target = 1.0;
	QString label;
};

/* One link in the operator's play queue. GET /reactions. */
struct Reaction {
	QString id;
	QString title;
	QString status;
};

/* One audio input, as the routing model describes it: what it is, where it actually ends up, and
   whether anything about it is a violation. From GET /audio-routing's `summary.sources`, which
   lib/audio-routing.mjs derives ONCE for all three surfaces precisely so they cannot answer
   "where is my audio going" differently. */
struct Channel {
	QString name;
	QString from;
	QStringList to;
	int alignMs = 0;
	bool muted = false;
	/* "bad" | "warn" | empty. The rig's word, not a colour — the theme decides the colour. */
	QString tone;
	QString note;
};

/* A process tap behind an application-audio input.
   ⚠️ `ok` AND `verified` ARE DIFFERENT QUESTIONS AND MUST NOT COLLAPSE. server.mjs states it:
   "apptap's exit 0 proves the aggregate device exists and NOTHING about whether audio flows
   through it... an ungranted tap is digital silence with noErr everywhere — no dialog, no log,
   every light green, a dead music bed on stream." */
struct Tap {
	QString deviceName;
	QString input;
	bool ok = false;
	bool verified = false;
	QString label;
	QString tone;
};

/* ── freshness, per block ────────────────────────────────────────────────────────────────────────
 * A block that has never answered and a block that answered ninety seconds ago are different rig
 * states and the panel draws them differently. `at` is a monotonic ms stamp from the client's own
 * elapsed timer — never a wall clock, because a panel that subtracts two epochs across an NTP step
 * reports an age that is wrong in the one direction that matters. */
struct Freshness {
	bool ever = false; /* has this block EVER been filled in this session */
	qint64 at = 0;     /* monotonic ms of the last good reply; 0 when !ever */
	QString fault;     /* why the last attempt failed, in the server's own words. Empty = fine. */
};

/* ── the safety block: GET /hub, one instant ─────────────────────────────────────────────────── */
struct Hub {
	Freshness fresh;
	/* `ok` is not decoration and lib/hub.mjs says why: without it "the panel cannot tell this
	   document from the 404 body that an older events server would answer with, and 'every key I
	   wanted is missing' is exactly what both look like." */
	bool ok = false;
	qint64 rev = 0;

	Maybe<QString> scene;
	Maybe<bool> sceneOk;
	Maybe<QList<Scene>> scenes;
	Maybe<QString> mask; /* "vision" | "block" */
	Maybe<bool> live;
	Maybe<qint64> sinceMs;
	Maybe<double> kbps;
	Maybe<double> droppedPct;
	Maybe<double> cpu;
	Maybe<double> fps;
	Maybe<qint64> obsAgeMs;

	Maybe<QString> hold; /* WHICH scene is the privacy hold — config's answer, never ours */
	Maybe<bool> held;

	Maybe<QString> phase;
	Maybe<qint64> inMs;

	Maybe<QString> emote;
	Maybe<QStringList> emotes;

	/* The audio summary the ESP32 panels also draw. The detail lives in `Model::audio`; these four
	   are here because they arrive in the SAME instant as the mask and the scene. */
	Maybe<QString> audioMon;
	Maybe<int> audioLatMs;
	Maybe<int> audioBad;
	Maybe<QString> audioTaps;
};

/* ── moderation: GET /queue ──────────────────────────────────────────────────────────────────── */
struct Queue {
	Freshness fresh;
	bool ok = false;
	/* MASTER AUTO, reflected from the SERVER and never from what was last pressed — the deck flips
	   it too, and control.html's own note is that "a button reading a stale OFF while events are
	   flowing is worse than no button." */
	bool autoAll = false;
	QList<QueueItem> pending;
	QList<QueueItem> approved;

	/* ── the three facts that ride along on this one poll ──────────────────────────────────────
	 * server.mjs states why they are here rather than on /state: "goal / brbcam / lastScene ride
	 * along because this is the panel's ONE poll — the goal fields and the BRB button reflect
	 * server truth from here, and a second poll of /state just for them would double the request
	 * rate for nothing." The fork honours that: BRB and the goal are drawn from this block, not
	 * from a route of their own.
	 *
	 * ⚠️ AND /queue MAKES ZERO OBS CALLS, which is what makes it safe for the privacy block to
	 * depend on: BRB keeps answering when OBS is dead, exactly like the EVT dot on the console. */
	Maybe<bool> brb;
	Maybe<Goal> goal;
	/* The link currently on the REACTIONS panel, from `share.url`. Absent means the idle card is
	   up — never "" — because an empty box and a box showing nothing are the same pixels and
	   different rig states. */
	Maybe<QString> share;
};

/* ── the play queue: GET /reactions ──────────────────────────────────────────────────────────── */
struct Reactions {
	Freshness fresh;
	bool ok = false;
	QList<Reaction> items;
	Maybe<QString> currentId;
	QString currentTitle;
	int upNext = 0;
	/* CAN THIS ONE BE DRIVEN? The server's capability table answers, never a guess about the URL:
	   YouTube publishes a remote control, Twitch and Kick do not. When `control` is false the
	   transport row is REMOVED and `reason` is printed in its place — "four buttons that quietly do
	   nothing are found out mid-reaction, on air." */
	bool control = false;
	QString reason;
	/* What the WRAPPER last reported, upper-cased by the renderer, never what was last pressed. */
	QString state;
	/* Absent when the player will not volunteer a position. -10/+10 grey out rather than seeking
	   from a number nobody measured. */
	Maybe<double> position;
	double rate = 1.0;
	QList<double> rates;
};

/* ── the chat embed: GET /chat ───────────────────────────────────────────────────────────────── */
struct Chat {
	Freshness fresh;
	bool ok = false;
	QString url;
	QString source;
	/* "obs-down" | "no-url" — the server's own reason word, so the pane's placeholder can name the
	   actual state instead of saying "unavailable" for two very different problems. */
	QString reason;
};

/* ── the audio routing: GET /audio-routing ───────────────────────────────────────────────────── */
struct Audio {
	Freshness fresh;
	bool ok = false;
	QString monitor;
	int addedLatencyMs = 0;
	int slackMs = 0;
	QList<Channel> sources;
	QList<Tap> taps;
	int blocking = 0;
	int warnings = 0;
	/* False when the OBS mixer cache is empty — i.e. the routing model is describing a rig it has
	   never actually seen. A channel list drawn from that is a diagram, not a reading. */
	bool live = false;
};

/* ── the OBS mirror: GET /obs ────────────────────────────────────────────────────────────────────
 * ⚠️ THIS BLOCK EXISTS FOR ONE BIT AND THAT BIT HAS THREE STATES.
 *
 * `mirror` is not on /hub — the hub document is the ESP32 panels' whole world and the flip is not
 * one of their three actions — so the fork reads it here. server.mjs is explicit about the shape:
 * "null means 'no answer': the filter is not provisioned on every camera, the angles disagree, or
 * the link is down. The panel must render that as a disabled control, NOT as off — an off-looking
 * switch that cannot be turned on is the kind of thing an operator fights with mid-stream."
 *
 * ⚠️ AND THIS ROUTE IS POLLED AT 5s, NOT 1Hz, AND THAT IS AN OUTAGE FIX RATHER THAN A BUDGET.
 * control.html carries the note: sustained 1Hz polling is what took OBS down repeatedly. /obs
 * itself answers from an event-driven cache with zero OBS calls, but the fork matches the console's
 * cadence anyway so that the two clients put the same load on the same socket. See DIVISOR[] in
 * QCiRigClient.cpp. */
struct Obs {
	Freshness fresh;
	bool ok = false;
	/* ABSENT IS THE THIRD STATE. Present-and-true only when EVERY camera carrying the filter has it
	   on; `mixed` names a disagreement between angles, which is a different sentence from "off". */
	Maybe<bool> mirror;
	Maybe<bool> mirrorPresent;
	bool mirrorMixed = false;
	/* Why the route said no, in its own words — "OBS unreachable", "mirror spec unavailable". */
	QString error;
};

/* THE WHOLE WORLD, AS ONE VALUE. Widgets take a `const Model &` and return void; none of them owns
   a network object, a timer, or a copy of any field below. */
struct Model {
	Hub hub;
	Queue queue;
	Reactions reactions;
	Chat chat;
	Audio audio;
	Obs obs;
};

/* ── parsing ─────────────────────────────────────────────────────────────────────────────────────
 * Each of these takes the reply body and returns a fresh block. They are PURE and they are the only
 * place a JSON key name is spelled in this fork — a second speller is the drift qci-rig's
 * lib/contracts.test.mjs exists to catch, and the fork is the copy with no way to notice.
 *
 * ⚠️ A KEY THAT IS PRESENT BUT OF THE WRONG TYPE IS TREATED AS ABSENT, not as its zero. QJsonValue
 * is happy to hand back 0 for a string and false for a number; taking those would reintroduce
 * exactly the defaults the optionals exist to prevent, one QJsonValue::toInt() at a time. */
Hub ParseHub(const QJsonObject &o);
Queue ParseQueue(const QJsonObject &o);
Reactions ParseReactions(const QJsonObject &o);
Chat ParseChat(const QJsonObject &o);
Audio ParseAudio(const QJsonObject &o);
Obs ParseObs(const QJsonObject &o);

/* ── rendering helpers, shared so two panes cannot format the same fact two ways ──────────────── */

/** Uptime as H:MM:SS from a duration. Never from two clocks subtracted here — the rig publishes a
 *  DURATION for the same reason the ESP32 needs one: nothing on this path is allowed to assume the
 *  two ends agree about what time it is. */
QString FormatDuration(qint64 ms);

/** A money-ish amount with its currency, or an empty string when there is no amount. Empty, not
 *  "$0" — a follow with no money attached has no dollar figure and printing one invents a donation.
 */
QString FormatAmount(const Maybe<double> &amount, const QString &currency);

/** The one place "this block is N seconds old" becomes words. */
QString FormatAge(qint64 ms);

/** A goal figure as whole dollars. The goal is the ONE quantity in this application that is allowed
 *  to be pink, and it is pink because it is money — so it is formatted in one place rather than in
 *  the three widgets that draw it. */
QString FormatMoney(double amount);

} // namespace QCiRig
