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

#include "QCiRigClient.hpp"
#include "QCiRigDocks.hpp"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPair>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QWidget>

#include "moc_QCiRigClient.cpp"

using namespace QCiRig;

namespace {

/* ── THE CADENCE ─────────────────────────────────────────────────────────────────────────────────
 * ONE TICK. Everything else is a divisor of it, so there is exactly one timer in this fork talking
 * to the rig and the whole schedule can be read in five lines.
 *
 * 1000ms is not a guess: it is the rate the ESP32 firmware polls /hub at, and the rate lib/hub.mjs
 * built its `rev` change token around ("a panel that polls at 1Hz forever repaints only when
 * something actually moved"). Matching it means the fork and the boards put the same load on the
 * same loopback server and see the same document age. */
constexpr int TICK_MS = 1000;

/* Per-surface divisors, in ticks. The two that are not 1 are the two whose contents move slowly and
 * whose replies are the largest:
 *   CHAT  the embed URL changes only when the program scene's chat source changes, and re-reading
 *         it more often buys nothing — the pane deliberately does not reload the view unless the
 *         URL actually differs, because reloading an embed restarts somebody else's player.
 *   AUDIO the routing model is rebuilt from the OBS mixer cache on every request; nothing in it
 *         moves between scene changes, and a per-second rebuild of a graph is cost with no reader.
 *   OBS   ⚠️ FIVE SECONDS IS AN OUTAGE FIX, NOT A BUDGET. control.html:2027 is
 *         `setInterval(pollObs, 5000)` and carries its own note about why 1s was abandoned:
 *         sustained polling is what took OBS down repeatedly ("with every client stopped it
 *         reconnects in 0ms; with them running it stops completing handshakes at all"). The route
 *         itself is answered from an event-driven cache with zero OBS calls per request, so the
 *         cost is not in the handler — it is in the socket. The fork matches the console's cadence
 *         so that two clients on the same loopback server put the same load on the same socket and
 *         see the same document age. DO NOT LOWER THIS to make the mirror switch feel snappier;
 *         actMirror() chains its own confirmation read instead, which is the thing that actually
 *         wanted fixing (a timer-based re-poll measurably left that button lying for ~5.7s).
 */
constexpr int DIVISOR[QCiRigClient::SurfaceCount] = {1, 1, 1, 5, 5, 5};

/* ⚠️ EVERY REQUEST HAS A HARD END, AND THAT IS A MEASURED REQUIREMENT RATHER THAN HYGIENE.
 * events/control.html: "MEASURED, with OBS closed: /micfx?levels=1 and /camfx never answer at all —
 * the events server holds the request open instead of failing it (curl -m 8 returns nothing)."
 * A client with no abort parks a connection forever on exactly the failure it most needs to survive.
 * Implemented with our own timer rather than QNetworkRequest::setTransferTimeout so there is one
 * mechanism to read and no Qt-version-dependent overload in the path. */
constexpr int ABORT_MS = 1500;

/* Three strikes, then one probe per five seconds. Same shape and same numbers as the console's
 * recovered poll, so a rig that is down costs the same either way you are looking at it. */
constexpr int FAIL_STRIKES = 3;
constexpr int BACKOFF_MS = 5000;

/* ─────────────────────────────────────────────────────────────────────────────────────────────
 * THE ROUTES. This function is the ONLY place in this fork that spells one, and the base address
 * is not written here at all — QCiRigDocks::BaseUrl() owns it, because it is a runtime setting and
 * qci-rig/lib/studiodocks.test.mjs asserts the fork spells the rig's address in exactly one file.
 *
 * A route name typed a second time is the drift qci-rig/lib/contracts.test.mjs exists to catch, and
 * a compiled binary in a different repository is the copy with no way to notice when it goes stale.
 */
QString RouteFor(QCiRigClient::Surface surface)
{
	switch (surface) {
	case QCiRigClient::SurfaceHub:
		return QStringLiteral("/hub");
	case QCiRigClient::SurfaceQueue:
		return QStringLiteral("/queue");
	case QCiRigClient::SurfaceReactions:
		return QStringLiteral("/reactions");
	case QCiRigClient::SurfaceChat:
		return QStringLiteral("/chat");
	case QCiRigClient::SurfaceAudio:
		return QStringLiteral("/audio-routing");
	case QCiRigClient::SurfaceObs:
		return QStringLiteral("/obs");
	case QCiRigClient::SurfaceCount:
		break;
	}
	return QString();
}

const char *NameFor(QCiRigClient::Surface surface)
{
	switch (surface) {
	case QCiRigClient::SurfaceHub:
		return "hub";
	case QCiRigClient::SurfaceQueue:
		return "queue";
	case QCiRigClient::SurfaceReactions:
		return "reactions";
	case QCiRigClient::SurfaceChat:
		return "chat";
	case QCiRigClient::SurfaceAudio:
		return "audio";
	case QCiRigClient::SurfaceObs:
		return "obs";
	case QCiRigClient::SurfaceCount:
		break;
	}
	return "?";
}

/* One query value, percent-encoded exactly once. QUrlQuery is used rather than string arithmetic
   because scene names carry spaces and parentheses ("LIVE (Delayed Output)") and an emote name or a
   reaction URL is arbitrary text — a hand-built query is how a scene switch silently asks for a
   scene that does not exist. */
QString WithQuery(const QString &route, const QList<QPair<QString, QString>> &args)
{
	if (args.isEmpty()) {
		return route;
	}
	QUrlQuery query;
	for (const QPair<QString, QString> &kv : args) {
		query.addQueryItem(kv.first, kv.second);
	}
	return route + QStringLiteral("?") + query.toString(QUrl::FullyEncoded);
}

} // namespace

/* ── construction ────────────────────────────────────────────────────────────────────────────── */

QCiRigClient *QCiRigClient::Get()
{
	static QPointer<QCiRigClient> instance;
	if (!instance) {
		instance = new QCiRigClient(QCoreApplication::instance());
	}
	return instance;
}

QCiRigClient::QCiRigClient(QObject *parent) : QObject(parent)
{
	m_net = new QNetworkAccessManager(this);
	/* NO SYSTEM PROXY ON LOOPBACK. macOS hands Qt whatever proxy the network profile names, and a
	   corporate or VPN profile that does not exempt 127.0.0.1 turns every request here into a
	   connection to a machine that has never heard of this rig — which fails slowly and looks
	   exactly like the events server being down. */
	m_net->setProxy(QNetworkProxy::NoProxy);

	m_clock.start();

	m_tick = new QTimer(this);
	m_tick->setInterval(TICK_MS);
	m_tick->setTimerType(Qt::CoarseTimer);
	connect(m_tick, &QTimer::timeout, this, &QCiRigClient::tick);
}

void QCiRigClient::start()
{
	if (!m_tick->isActive()) {
		m_tick->start();
		/* One immediate pass so a freshly opened panel has a correct first frame instead of a
		   second of placeholders. The console learned the same thing and calls it "one prime". */
		refresh();
	}
}

qint64 QCiRigClient::nowMs() const
{
	return m_clock.elapsed();
}

void QCiRigClient::wantSurface(Surface surface, QWidget *pane)
{
	if (surface >= SurfaceCount || !pane) {
		return;
	}
	m_surface[surface].panes.append(QPointer<QWidget>(pane));
}

bool QCiRigClient::surfaceWanted(Surface surface) const
{
	/* THE SAFETY BLOCK IS NEVER GATED. Everything the operator needs in order to answer "is my face
	   on air right now" arrives on /hub, and the pane that carries the panic control is not
	   hideable — so there is no state in which skipping this poll would be correct. */
	if (surface == SurfaceHub) {
		return true;
	}
	const SurfaceState &st = m_surface[surface];
	for (const QPointer<QWidget> &pane : st.panes) {
		if (pane && pane->isVisible()) {
			return true;
		}
	}
	return false;
}

/* ── the tick ────────────────────────────────────────────────────────────────────────────────── */

void QCiRigClient::tick()
{
	m_ticks++;
	const bool force = m_forceAll;
	m_forceAll = false;

	for (int i = 0; i < SurfaceCount; i++) {
		const Surface surface = static_cast<Surface>(i);
		SurfaceState &st = m_surface[i];
		st.ticksSincePoll++;
		if (!surfaceWanted(surface)) {
			continue;
		}
		if (!force && st.ticksSincePoll < DIVISOR[i]) {
			continue;
		}
		poll(surface);
	}
}

void QCiRigClient::refresh()
{
	m_forceAll = true;
	tick();
}

void QCiRigClient::poll(Surface surface)
{
	SurfaceState &st = m_surface[surface];

	/* ONE IN FLIGHT. Not an optimisation — see ABORT_MS above. A second request issued while the
	   first is parked on a route the server is holding open is how six connections become zero. */
	if (st.inFlight) {
		return;
	}
	const qint64 now = m_clock.elapsed();
	if (st.fails >= FAIL_STRIKES && now < st.nextAt) {
		return;
	}
	st.ticksSincePoll = 0;
	get(surface, RouteFor(surface));
}

void QCiRigClient::get(Surface surface, const QString &route)
{
	const QUrl url(QCiRigDocks::BaseUrl() + route);
	if (!url.isValid()) {
		return;
	}

	QNetworkRequest req(url);
	req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);

	QNetworkReply *reply = m_net->get(req);
	m_surface[surface].inFlight = reply;

	/* THE ABORT. Parented to the reply so it dies with it; abort() makes finished() fire with
	   OperationCanceledError, which is the same path as any other failure and needs no second
	   branch. */
	QTimer *cutoff = new QTimer(reply);
	cutoff->setSingleShot(true);
	cutoff->setInterval(ABORT_MS);
	connect(cutoff, &QTimer::timeout, reply, [reply]() {
		if (reply->isRunning()) {
			reply->abort();
		}
	});
	cutoff->start();

	connect(reply, &QNetworkReply::finished, this, [this, surface, reply]() { finish(surface, reply); });
}

void QCiRigClient::finish(Surface surface, QNetworkReply *reply)
{
	reply->deleteLater();
	SurfaceState &st = m_surface[surface];
	st.inFlight.clear();

	const qint64 now = m_clock.elapsed();

	auto fail = [&](const QString &why) {
		st.fails++;
		if (st.fails >= FAIL_STRIKES) {
			st.nextAt = now + BACKOFF_MS;
		}
		/* THE BLOCK KEEPS ITS LAST GOOD CONTENT AND GAINS A FAULT — it is not blanked.
		   A queue that vanishes because one poll timed out is a queue the operator thinks they
		   have cleared. The pane prints the fault and the age beside the stale content, which is
		   the honest rendering of "this is what the rig last said, and that was a while ago". */
		switch (surface) {
		case SurfaceHub:
			m_model.hub.fresh.fault = why;
			break;
		case SurfaceQueue:
			m_model.queue.fresh.fault = why;
			break;
		case SurfaceReactions:
			m_model.reactions.fresh.fault = why;
			break;
		case SurfaceChat:
			m_model.chat.fresh.fault = why;
			break;
		case SurfaceAudio:
			m_model.audio.fresh.fault = why;
			break;
		case SurfaceObs:
			m_model.obs.fresh.fault = why;
			break;
		case SurfaceCount:
			break;
		}
		emit changed();
	};

	if (reply->error() != QNetworkReply::NoError) {
		fail(reply->errorString());
		return;
	}

	const QByteArray body = reply->readAll();
	QJsonParseError parseError{};
	const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
	if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
		/* A body that is not an object is very often the events server's 404 page or a proxy's
		   error page, and both of those parse as "not what I asked for" rather than as a rig
		   state. Named as such rather than swallowed. */
		fail(QStringLiteral("bad reply from /%1").arg(QString::fromUtf8(NameFor(surface))));
		return;
	}

	const QJsonObject o = doc.object();
	Freshness fresh;
	fresh.ever = true;
	fresh.at = now;

	switch (surface) {
	case SurfaceHub:
		m_model.hub = ParseHub(o);
		m_model.hub.fresh = fresh;
		break;
	case SurfaceQueue:
		m_model.queue = ParseQueue(o);
		m_model.queue.fresh = fresh;
		break;
	case SurfaceReactions:
		m_model.reactions = ParseReactions(o);
		m_model.reactions.fresh = fresh;
		break;
	case SurfaceChat:
		m_model.chat = ParseChat(o);
		m_model.chat.fresh = fresh;
		break;
	case SurfaceAudio:
		m_model.audio = ParseAudio(o);
		m_model.audio.fresh = fresh;
		break;
	case SurfaceObs:
		m_model.obs = ParseObs(o);
		m_model.obs.fresh = fresh;
		break;
	case SurfaceCount:
		break;
	}

	st.fails = 0;
	st.nextAt = 0;
	emit changed();
}

/* ── writes ──────────────────────────────────────────────────────────────────────────────────── */

void QCiRigClient::write(const QString &route, bool post, const QString &okText,
			 std::function<void(bool ok, const QJsonObject &body)> onReply)
{
	const QUrl url(QCiRigDocks::BaseUrl() + route);
	if (!url.isValid()) {
		return;
	}

	QNetworkRequest req(url);
	req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);

	/* POST for everything /hub/action serves, GET for the loopback routes that were built as GETs
	   before that convention existed. The rig's reason for the POST is worth keeping in view: "a
	   state-changing GET is reachable from an <img> tag in any page the operator happens to open on
	   this Mac." */
	QNetworkReply *reply = post ? m_net->post(req, QByteArray()) : m_net->get(req);

	QTimer *cutoff = new QTimer(reply);
	cutoff->setSingleShot(true);
	cutoff->setInterval(ABORT_MS);
	connect(cutoff, &QTimer::timeout, reply, [reply]() {
		if (reply->isRunning()) {
			reply->abort();
		}
	});
	cutoff->start();

	connect(reply, &QNetworkReply::finished, this, [this, reply, okText, onReply]() {
		reply->deleteLater();

		/* ⚠️ SUCCESS IS ESTABLISHED POSITIVELY HERE — IT IS NEVER WHAT IS LEFT OVER. This is the
		   ONE reporting path for every write verb, and the loudest sentence it can print is
		   actHold()'s "PRIVACY HOLD — CUT, MASKED". Printing that merely because nothing
		   contradicted it is how an operator reads CUT off this panel while the camera is still
		   live: a 404 from a mistyped BaseUrl or from route drift, a 500 out of a handler, an HTML
		   error page, an aborted body — none of those carry `ok` at all, and "no ok" used to fall
		   into the success branch below. So okText is spoken only when all four facts hold: the
		   transport succeeded, the status is 2xx, the body IS a JSON object, and that object says
		   ok:true. Anything else is reported as a fault, exactly as the GET path in finish() has
		   always done with the same two failures. */
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
		const QJsonObject o = doc.isObject() ? doc.object() : QJsonObject();
		const QJsonValue ok = o.value(QLatin1String("ok"));

		/* The facts, named rather than inlined, because "why did the panel not print the success
		   line" has to be answerable by reading this. `confirmed` carries the body check too: a
		   reply that is not a JSON object was degraded to an empty one above, so `ok` is Undefined
		   there and isBool() is false — an HTML 404 page cannot confirm anything. */
		const bool answered = reply->error() == QNetworkReply::NoError;
		const bool http2xx = status >= 200 && status < 300;
		const bool confirmed = ok.isBool() && ok.toBool();

		/* ⚠️ A REFUSAL IS REPORTED IN THE SERVER'S OWN WORDS, NEVER SWALLOWED. control.html:
		   "a refusal here is not an error to swallow: 'PAUSE on a Twitch clip' comes back
		   {ok:false} carrying the reason that platform cannot be driven, and that sentence is the
		   whole point of having asked." The rig also splits WHOSE fault it is — a reply carrying
		   `fault` means the rig cannot do this right now (grey the control), one without means the
		   request was refused (the control works, the ask was wrong) — so the word is carried
		   through instead of being flattened to "error".

		   READ BEFORE THE STATUS IS JUDGED, deliberately: a refusal arrives as a 400 as often as a
		   200. lib/hub.mjs: "the route turns the same distinction into 200-with-ok:false versus
		   400", so the gate doing its job must print its sentence rather than be flattened into a
		   transport fault. */
		if (ok.isBool() && !ok.toBool()) {
			const QJsonValue err = o.value(QLatin1String("error"));
			const QString why = err.isString() ? err.toString() : QStringLiteral("no reason given");
			const QJsonValue fault = o.value(QLatin1String("fault"));
			const QString whose = fault.isString() ? fault.toString().toUpper() : QString();
			emit say(whose.isEmpty() ? QStringLiteral("REFUSED: %1").arg(why.toUpper())
						 : QStringLiteral("%1 FAULT: %2").arg(whose, why.toUpper()));
		} else if (confirmed && answered && http2xx) {
			/* `ok:true` is the rig's own confirmation and every route this client writes to carries
			   one — hub.mjs states why it is not decoration: "without it the panel cannot tell this
			   document from the 404 body (`{"error":"not found"}`) that an older events server ...
			   would answer with, and 'every key I wanted is missing' is exactly what both look
			   like." Present-and-true is therefore the only thing that licenses okText. */
			if (!okText.isEmpty()) {
				emit say(okText);
			}
		} else if (status == 0) {
			/* No status code at all: nothing answered — connection refused, or our own ABORT_MS
			   cutoff fired on a request the events server was holding open. */
			emit say(QStringLiteral("LINK FAULT: RIG UNREACHABLE — ACTION UNCONFIRMED"));
		} else if (!http2xx) {
			emit say(QStringLiteral("LINK FAULT: RIG ANSWERED %1 — ACTION UNCONFIRMED").arg(status));
		} else {
			/* 2xx, but not an answer this client can read: an empty or truncated body, an object
			   with no `ok`, or a transport that faulted after the headers arrived. It may have
			   worked; nothing here knows that it did, and the one thing this line must never do is
			   say it did. */
			emit say(QStringLiteral("LINK FAULT: UNREADABLE REPLY — ACTION UNCONFIRMED"));
		}
		/* THE WORDING IS LOAD-BEARING, not free-form: the pinned readout paints its alert tone only
		   for text that starts with REFUSED or contains FAULT (QCiRigPanes, where the say() signal
		   lands), so a failure worded any other way would render in the same calm tone as the
		   success it replaced. FAULT is in every failure branch above for that reason. */

		/* ⚠️ HANDED THE SAME JUDGEMENT THIS FUNCTION USED, NOT A SECOND ONE. There is exactly one
		   definition of "the rig said yes" in this fork — the four facts named above — and a caller
		   that re-derived it from `body` would be free to be more generous. The one caller there is
		   renders the word CUT. */
		if (onReply) {
			onReply(confirmed && answered && http2xx, o);
		}

		/* THE SERVER IS THE AUTHORITY, ALWAYS. Nothing is rendered from what was just pressed —
		   re-read and draw what the rig actually did. The deck, the CLI, the wrapper page and a
		   video simply ENDING all move this same state with nobody touching this panel. */
		refresh();
	});
}

/* ── the six verbs the LAN panels also have ──────────────────────────────────────────────────── */

void QCiRigClient::actHold()
{
	/* ONE PRESS, ONE POST, ONE DECISION — and the decision is the rig's. hubAction() answers `hold`
	   with scene + cut + mask together precisely because "a panic that is two requests is a panic
	   whose second half can be the one that does not arrive." Nothing here composes it.
	 *
	 * ⚠️ THE OK TEXT NO LONGER CLAIMS THE CUT, because this client cannot know it from a 2xx. The
	 * rig's reply carries `vcam` as one of three words and only one of them means the virtual camera
	 * came off air; "PRIVACY HOLD — CUT, MASKED" printed on all three is the same class of lie the
	 * write path was already rewritten once to stop telling. The words go out on holdAnswered(),
	 * where the panel renders them as three different things. */
	write(WithQuery(QStringLiteral("/hub/action"), {{QStringLiteral("a"), QStringLiteral("hold")}}), true,
	      QStringLiteral("PRIVACY HOLD — ASKED"), [this](bool ok, const QJsonObject &body) {
		      const QJsonValue vcam = body.value(QLatin1String("vcam"));
		      const QJsonValue scene = body.value(QLatin1String("scene"));
		      emit holdAnswered(ok, vcam.isString() ? vcam.toString() : QString(),
					scene.isString() ? scene.toString() : QString());
	      });
}

void QCiRigClient::actBrb(bool on)
{
	/* NAMED DIRECTION. Same rule as the mask and the mute: this panel renders from a document up to
	   a second old, and `rig brbcam` is also driven by the CLI and the deck, so a toggle computed
	   from what is on screen uncovers a camera the operator just covered. */
	write(WithQuery(QStringLiteral("/brbcam"),
			{{QStringLiteral("on"), on ? QStringLiteral("1") : QStringLiteral("0")}}),
	      false, on ? QStringLiteral("BRB ON") : QStringLiteral("BRB OFF"));
}

void QCiRigClient::actMirror(bool on)
{
	/* ⚠️ THE CONFIRMATION READ IS CHAINED OFF THE WRITE, NOT LEFT TO THE NEXT /obs TICK.
	 *
	 * /obs is polled every 5s (see DIVISOR) and control.html measured what that costs on this
	 * control: a timer-based re-poll left the button showing the OLD state for ~5.7s, which on a
	 * flip switch reads as a press that did nothing and invites a second press that undoes it. So
	 * the write's own completion schedules a forced read — +250ms, because OBS broadcasts
	 * SourceFilterEnableStateChanged back to the events server asynchronously and reading sooner
	 * reads the state before the change lands.
	 *
	 * refresh() is already called by write() on completion; this adds the second, later read that
	 * catches the event. Both are forced, so the divisor does not defer them. */
	write(WithQuery(QStringLiteral("/mirror"),
			{{QStringLiteral("enabled"), on ? QStringLiteral("1") : QStringLiteral("0")}}),
	      false, on ? QStringLiteral("MIRROR ON") : QStringLiteral("MIRROR OFF"),
	      [this](bool, const QJsonObject &) { QTimer::singleShot(250, this, [this]() { refresh(); }); });
}

void QCiRigClient::actInjectPaidTestItem()
{
	/* ⚠️ THIS IS NOT A DRY RUN. GET /event puts a real superchat on the moderation queue and the
	   goal counts it (isMoney → state.goal.current += amount). The rig has no test mode and adding
	   one would be a second code path through the moderation gate, which is the last place a fork
	   should carry a branch the rig does not have. So it stays real, and every surface that offers
	   it says so — see the button's label. */
	write(WithQuery(QStringLiteral("/event"),
			{{QStringLiteral("type"), QStringLiteral("superchat")},
			 {QStringLiteral("platform"), QStringLiteral("youtube")},
			 {QStringLiteral("who"), QStringLiteral("TEST")},
			 {QStringLiteral("amount"), QStringLiteral("5")},
			 {QStringLiteral("text"), QStringLiteral("qci-studio operator panel test")}}),
	      false, QStringLiteral("INJECTED A REAL $5 ITEM"));
}

void QCiRigClient::actGoal(double current, double target, const QString &label)
{
	/* The rig clamps: current = max(0, n||0), target = max(1, n||1), label sliced to 24. Nothing is
	   clamped here — a second clamp is a second opinion about what is legal, and the whole reason
	   this panel talks to the events server rather than to OBS is that the server already decides. */
	QList<QPair<QString, QString>> args{{QStringLiteral("current"), QString::number(current, 'f', 2)},
					    {QStringLiteral("target"), QString::number(target, 'f', 2)}};
	if (!label.isEmpty()) {
		args.append({QStringLiteral("label"), label});
	}
	write(WithQuery(QStringLiteral("/goal"), args), false, QStringLiteral("GOAL SET"));
}

void QCiRigClient::actGoalClear()
{
	/* CURRENT ONLY. The rig keeps the target and the label, because "clear" on a fundraising meter
	   means "start the run again", not "forget what we were raising for". */
	write(WithQuery(QStringLiteral("/goal"), {{QStringLiteral("current"), QStringLiteral("0")}}), false,
	      QStringLiteral("GOAL CLEARED"));
}

void QCiRigClient::actShare(const QString &url)
{
	/* http(s)-ONLY IS THE SERVER'S RULE AND IT STAYS THERE. This URL is pointed at a browser source
	   that renders ON STREAM, so a file:// or javascript: link here is a live source reading the
	   operator's disk in front of an audience — and the events server refuses both by regex. A
	   second check here would be a second place for that rule to be relaxed by somebody who did not
	   read this comment. */
	write(WithQuery(QStringLiteral("/share"), {{QStringLiteral("url"), url}}), false,
	      QStringLiteral("SHARED"));
}

void QCiRigClient::actShareClear()
{
	write(WithQuery(QStringLiteral("/share"), {{QStringLiteral("clear"), QStringLiteral("1")}}), false,
	      QStringLiteral("SHARE CLEARED"));
}

void QCiRigClient::actArm(int minutes)
{
	/* ⚠️ ZERO IS A VALUE, NOT A MISSING ONE, AND IT MEANS START NOW. events/server.mjs:
	   "`Number(q.min) || DEFAULT` swallows ZERO — 0 is falsy, so 'start now' silently became a
	   15-minute countdown. Test the parse, not the truthiness." The parameter is therefore always
	   sent, including as "0"; omitting it when minutes==0 would hand the rig `undefined`, which is
	   exactly the NaN case its default fires on. */
	write(WithQuery(QStringLiteral("/start"), {{QStringLiteral("min"), QString::number(minutes)}}), false,
	      minutes == 0 ? QStringLiteral("STARTING NOW") : QStringLiteral("ARMED %1 MIN").arg(minutes));
}

void QCiRigClient::actArmCancel()
{
	write(WithQuery(QStringLiteral("/start"), {{QStringLiteral("cancel"), QStringLiteral("1")}}), false,
	      QStringLiteral("START CANCELLED"));
}

void QCiRigClient::actMask(const QString &mode)
{
	/* NO TOGGLE, and the reason is specific to this control: "a toggle re-reads the mask at PRESS
	   time from a document fetched up to a second ago, so the thumb that meant 'hide me' can un-hide
	   instead." The direction is fixed when the button is DRAWN. */
	write(WithQuery(QStringLiteral("/hub/action"),
			{{QStringLiteral("a"), QStringLiteral("mask")}, {QStringLiteral("v"), mode}}),
	      true, QStringLiteral("MASK %1").arg(mode.toUpper()));
}

void QCiRigClient::actScene(const QString &realSceneName)
{
	write(WithQuery(QStringLiteral("/hub/action"),
			{{QStringLiteral("a"), QStringLiteral("scene")}, {QStringLiteral("n"), realSceneName}}),
	      true, QStringLiteral("SCENE %1").arg(realSceneName.toUpper()));
}

void QCiRigClient::actStream(bool on)
{
	write(WithQuery(QStringLiteral("/hub/action"),
			{{QStringLiteral("a"), QStringLiteral("stream")},
			 {QStringLiteral("v"), on ? QStringLiteral("on") : QStringLiteral("off")}}),
	      true, on ? QStringLiteral("STREAM START ASKED") : QStringLiteral("STREAM STOP ASKED"));
}

void QCiRigClient::actEmote(const QString &name)
{
	write(WithQuery(QStringLiteral("/hub/action"),
			{{QStringLiteral("a"), QStringLiteral("emote")}, {QStringLiteral("e"), name}}),
	      true, QStringLiteral("QC %1").arg(name.toUpper()));
}

void QCiRigClient::actGo()
{
	write(WithQuery(QStringLiteral("/hub/action"), {{QStringLiteral("a"), QStringLiteral("go")}}), true,
	      QStringLiteral("GO"));
}

/* ── moderation: loopback only, deliberately ─────────────────────────────────────────────────── */

void QCiRigClient::actApprove(qint64 id)
{
	write(WithQuery(QStringLiteral("/approve"), {{QStringLiteral("id"), QString::number(id)}}), false,
	      QStringLiteral("APPROVED"));
}

void QCiRigClient::actReject(qint64 id)
{
	write(WithQuery(QStringLiteral("/reject"), {{QStringLiteral("id"), QString::number(id)}}), false,
	      QStringLiteral("REJECTED"));
}

void QCiRigClient::actApproveAll()
{
	/* One request per item, against the ids the LAST REPLY carried — never against a list this
	   client accumulated. An id that has already been actioned answers {ok:false,"no such item"},
	   which is reported and is harmless. */
	for (const QueueItem &it : m_model.queue.pending) {
		actApprove(it.id);
	}
}

void QCiRigClient::actAutoToggle()
{
	/* NO ARGUMENT — the toggle is decided server-side, and that is deliberate rather than lazy.
	   control.html: "Sending an explicit on/off from what the button currently READS would race the
	   deck: if it flipped this a moment ago our label is stale and we would command the state it is
	   already in." */
	write(QStringLiteral("/auto"), false, QStringLiteral("AUTO TOGGLED"));
}

/* ── the reaction queue and its transport ────────────────────────────────────────────────────── */

void QCiRigClient::actReactionAdd(const QString &url)
{
	write(WithQuery(QStringLiteral("/reactions/add"), {{QStringLiteral("url"), url}}), false,
	      QStringLiteral("QUEUED"));
}

void QCiRigClient::actReactionPlay(const QString &id)
{
	const QList<QPair<QString, QString>> args =
		id.isEmpty() ? QList<QPair<QString, QString>>{} : QList<QPair<QString, QString>>{{QStringLiteral("id"), id}};
	write(WithQuery(QStringLiteral("/reactions/play"), args), false, QStringLiteral("PLAY"));
}

void QCiRigClient::actReactionRemove(const QString &id)
{
	write(WithQuery(QStringLiteral("/reactions/remove"), {{QStringLiteral("id"), id}}), false,
	      QStringLiteral("REMOVED"));
}

void QCiRigClient::actReactionMove(const QString &id, int to)
{
	/* Index-based, "because that is what the operator can see: the row moves to where they pointed.
	   The server clamps, so a press on the top row's UP is a no-op rather than a refusal." */
	write(WithQuery(QStringLiteral("/reactions/reorder"),
			{{QStringLiteral("id"), id}, {QStringLiteral("to"), QString::number(to)}}),
	      false, QStringLiteral("MOVED"));
}

void QCiRigClient::actReactionPrev()
{
	write(QStringLiteral("/reactions/prev"), false, QStringLiteral("BACK"));
}

void QCiRigClient::actReactionNext()
{
	write(QStringLiteral("/reactions/next"), false, QStringLiteral("NEXT"));
}

void QCiRigClient::actReactionPause()
{
	write(QStringLiteral("/reactions/pause"), false, QStringLiteral("PAUSE"));
}

void QCiRigClient::actReactionResume()
{
	/* Resume is /reactions/play with NO id — the same route that starts a chosen item, which is why
	   PLAY and RESUME are one verb on the rig and two buttons here. */
	actReactionPlay(QString());
}

void QCiRigClient::actReactionSeekBy(double seconds)
{
	/* ⚠️ A RELATIVE SEEK NEEDS AN ABSOLUTE POSITION, AND THERE IS NOT ALWAYS ONE. The rig reports
	   t:null when the player will not volunteer currentTime; seeking from a number nobody measured
	   is a scrub bar that lies, so this refuses rather than guessing zero. The buttons are disabled
	   in that state too — this is the second gate, not the first. */
	if (!m_model.reactions.position) {
		emit say(QStringLiteral("REFUSED: NO PLAYER POSITION TO SEEK FROM"));
		return;
	}
	const double target = qMax(0.0, *m_model.reactions.position + seconds);
	write(WithQuery(QStringLiteral("/reactions/seek"),
			{{QStringLiteral("t"), QString::number(target, 'f', 1)}}),
	      false, QStringLiteral("SEEK"));
}

void QCiRigClient::actReactionRateStep()
{
	/* CYCLES rather than opening a picker. The server SNAPS whatever it is sent to a rate the player
	   will actually honour, so this list being the same list is a convenience, not a contract. */
	const QList<double> &rates = m_model.reactions.rates;
	if (rates.isEmpty()) {
		emit say(QStringLiteral("REFUSED: THIS PLAYER PUBLISHES NO RATES"));
		return;
	}
	/* qsizetype throughout: QList indexes in 64 bits and narrowing it here would be a warning the
	   build turns into an error. A rate list with more than 2^31 entries is not the concern; the
	   concern is that -Werror does not care. */
	qsizetype idx = rates.indexOf(m_model.reactions.rate);
	idx = (idx < 0) ? 0 : (idx + 1) % rates.size();
	write(WithQuery(QStringLiteral("/reactions/rate"),
			{{QStringLiteral("r"), QString::number(rates.at(idx), 'g', 4)}}),
	      false, QStringLiteral("RATE"));
}

/* ── audio ───────────────────────────────────────────────────────────────────────────────────── */

void QCiRigClient::actChannelMute(const QString &input, bool muted)
{
	/* NAMED DIRECTION, not a toggle, for the reason every other control here states: this panel
	   renders from a document up to a second old, and a toggle acted on a stale view unmutes the
	   microphone the operator just silenced. */
	write(WithQuery(QStringLiteral("/micfx"),
			{{QStringLiteral("input"), input},
			 {QStringLiteral("mute"), muted ? QStringLiteral("1") : QStringLiteral("0")}}),
	      false, muted ? QStringLiteral("MUTED %1").arg(input.toUpper())
			   : QStringLiteral("UNMUTED %1").arg(input.toUpper()));
}
