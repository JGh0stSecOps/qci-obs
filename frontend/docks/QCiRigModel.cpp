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

#include "QCiRigModel.hpp"

#include <QJsonArray>
#include <QJsonValue>

#include <cmath>

namespace QCiRig {

/* ─────────────────────────────────────────────────────────────────────────────────────────────
 * THE FIVE READERS BELOW ARE THE WHOLE REASON THIS FILE EXISTS.
 *
 * QJsonObject::value() returns a QJsonValue that will cheerfully answer toString() with "" for a
 * number, toBool() with false for a missing key and toDouble() with 0 for a string. Every one of
 * those answers is a DEFAULT, and the rig deleted the key from the wire specifically so that no
 * default would exist. So nothing in this file calls those converters without first proving the
 * key is present AND of the type it is supposed to be — an absent key and a key of the wrong shape
 * both come back as an empty optional, because a reply that has gone weird is not evidence about
 * the rig, it is evidence about the reply.
 */

static Maybe<QString> Str(const QJsonObject &o, const char *key)
{
	const QJsonValue v = o.value(QLatin1String(key));
	if (!v.isString()) {
		return std::nullopt;
	}
	return v.toString();
}

static Maybe<bool> Bool(const QJsonObject &o, const char *key)
{
	const QJsonValue v = o.value(QLatin1String(key));
	if (!v.isBool()) {
		return std::nullopt;
	}
	return v.toBool();
}

static Maybe<double> Num(const QJsonObject &o, const char *key)
{
	const QJsonValue v = o.value(QLatin1String(key));
	if (!v.isDouble()) {
		return std::nullopt;
	}
	const double d = v.toDouble();
	/* NaN and infinity cannot come out of a conforming JSON document, but they CAN come out of a
	   proxy that rewrote a body, and they render as "nan" in a label the operator then has to
	   interpret. Refused here rather than downstream in five formatters. */
	if (!std::isfinite(d)) {
		return std::nullopt;
	}
	return d;
}

static Maybe<qint64> Int(const QJsonObject &o, const char *key)
{
	const Maybe<double> d = Num(o, key);
	if (!d) {
		return std::nullopt;
	}
	return static_cast<qint64>(*d);
}

/* Plain accessors for the blocks where absence is NOT a state the panel renders differently — a
   moderation card with no `text` and a moderation card with an empty `text` are the same card. The
   optionals above are spent where the rig spent them, and nowhere else, so that reaching for one is
   a signal rather than a habit. */
static QString PlainStr(const QJsonObject &o, const char *key)
{
	const QJsonValue v = o.value(QLatin1String(key));
	return v.isString() ? v.toString() : QString();
}

static bool PlainBool(const QJsonObject &o, const char *key)
{
	const QJsonValue v = o.value(QLatin1String(key));
	return v.isBool() ? v.toBool() : false;
}

static int PlainInt(const QJsonObject &o, const char *key)
{
	const Maybe<qint64> i = Int(o, key);
	return i ? static_cast<int>(*i) : 0;
}

/* ── GET /hub ────────────────────────────────────────────────────────────────────────────────── */

Hub ParseHub(const QJsonObject &o)
{
	Hub h;
	h.ok = PlainBool(o, "ok");
	if (!h.ok) {
		/* Not a hub document. Everything stays absent — see the note on `ok` in the header: the
		   404 body of an events server that predates this route looks exactly like a document
		   whose every key happened to be missing, and only this marker tells them apart. */
		return h;
	}
	h.rev = PlainInt(o, "rev");

	h.scene = Str(o, "scene");
	h.sceneOk = Bool(o, "sceneOk");

	const QJsonValue scenes = o.value(QLatin1String("scenes"));
	if (scenes.isArray()) {
		QList<Scene> list;
		const QJsonArray arr = scenes.toArray();
		for (const auto v : arr) {
			if (!v.isObject()) {
				continue;
			}
			const QJsonObject so = v.toObject();
			const Maybe<QString> n = Str(so, "n");
			if (!n || n->isEmpty()) {
				/* A scene with no REAL name is a button that switches nothing. Dropped rather
				   than drawn from its label, which is the pretty string with the wrapper suffix
				   already removed — POSTing that back is the documented way to switch nothing. */
				continue;
			}
			Scene s;
			s.name = *n;
			const Maybe<QString> l = Str(so, "l");
			s.label = (l && !l->isEmpty()) ? *l : *n;
			list.append(s);
		}
		h.scenes = list;
	}

	/* ⚠️ THE MASK IS THE ONE FIELD WHERE A SLOPPY READ IS A PRIVACY FAILURE. The rig publishes it
	   only when it knows — "null = the QC Vision filter is not provisioned on this box, or the link
	   dropped mid-read... Rendering an unknown mask as 'block' would put a PRIVACY-ON badge over a
	   camera nobody has masked, which is the one lie this panel exists to not tell." So an
	   unrecognised word is absence too, not a third mode to guess at. */
	const Maybe<QString> mask = Str(o, "mask");
	if (mask && (*mask == QLatin1String("vision") || *mask == QLatin1String("block"))) {
		h.mask = *mask;
	}

	h.live = Bool(o, "live");
	h.sinceMs = Int(o, "sinceMs");
	h.kbps = Num(o, "kbps");
	h.droppedPct = Num(o, "droppedPct");
	h.cpu = Num(o, "cpu");
	h.fps = Num(o, "fps");
	h.obsAgeMs = Int(o, "obsAgeMs");

	h.hold = Str(o, "hold");
	h.held = Bool(o, "held");

	h.phase = Str(o, "phase");
	h.inMs = Int(o, "inMs");

	h.emote = Str(o, "emote");
	const QJsonValue emotes = o.value(QLatin1String("emotes"));
	if (emotes.isArray()) {
		QStringList names;
		const QJsonArray arr = emotes.toArray();
		for (const auto v : arr) {
			if (v.isString() && !v.toString().isEmpty()) {
				names.append(v.toString());
			}
		}
		/* Absent rather than empty when nothing survived, matching the rig: "an empty roster admits
		   nothing, so drawing zero buttons and drawing no grid at all are the same rig state and the
		   panel should say so once." */
		if (!names.isEmpty()) {
			h.emotes = names;
		}
	}

	const QJsonValue audio = o.value(QLatin1String("audio"));
	if (audio.isObject()) {
		const QJsonObject ao = audio.toObject();
		h.audioMon = Str(ao, "mon");
		const Maybe<qint64> lat = Int(ao, "lat");
		if (lat) {
			h.audioLatMs = static_cast<int>(*lat);
		}
		const Maybe<qint64> bad = Int(ao, "bad");
		if (bad) {
			h.audioBad = static_cast<int>(*bad);
		}
		h.audioTaps = Str(ao, "taps");
	}

	return h;
}

/* ── GET /queue ──────────────────────────────────────────────────────────────────────────────── */

static QueueItem ParseQueueItem(const QJsonObject &o)
{
	QueueItem it;
	const Maybe<qint64> id = Int(o, "id");
	it.id = id ? *id : 0;
	it.who = PlainStr(o, "who");
	it.amount = Num(o, "amount");
	it.currency = PlainStr(o, "currency");
	it.platform = PlainStr(o, "platform");
	it.kind = PlainStr(o, "kind");
	it.text = PlainStr(o, "text");
	it.tts = PlainBool(o, "tts");
	return it;
}

static QList<QueueItem> ParseQueueList(const QJsonValue &v)
{
	QList<QueueItem> out;
	if (!v.isArray()) {
		return out;
	}
	const QJsonArray arr = v.toArray();
	for (const auto e : arr) {
		if (!e.isObject()) {
			continue;
		}
		const QueueItem it = ParseQueueItem(e.toObject());
		/* An item with no id cannot be approved or rejected — every write route on this surface is
		   keyed by it — so a card for one is a card whose buttons are guaranteed to fail. */
		if (it.id != 0) {
			out.append(it);
		}
	}
	return out;
}

Queue ParseQueue(const QJsonObject &o)
{
	Queue q;
	/* GET /queue has no `ok` marker — it is older than that convention and it answers with the two
	   lists directly. control.html's applyQueue() uses exactly this test ("not a /queue reply --
	   ignore it") and the fork uses the same one rather than inventing a second rule. */
	if (!o.value(QLatin1String("pending")).isArray() || !o.value(QLatin1String("approved")).isArray()) {
		return q;
	}
	q.ok = true;
	q.autoAll = PlainBool(o, "autoAll");
	q.pending = ParseQueueList(o.value(QLatin1String("pending")));
	q.approved = ParseQueueList(o.value(QLatin1String("approved")));

	/* ⚠️ BRB IS READ AS AN OPTIONAL EVEN THOUGH THE RIG ALWAYS SENDS IT (`brbcam: !!state.brbcam`),
	   and the reason is the same one the whole model is built on: a key that is present-but-wrong-
	   type must not become `false`. A false BRB is drawn as "the camera is uncovered", which is a
	   claim about the operator's face made from a reply nobody checked. */
	q.brb = Bool(o, "brbcam");

	const QJsonValue goal = o.value(QLatin1String("goal"));
	if (goal.isObject()) {
		const QJsonObject go = goal.toObject();
		const Maybe<double> current = Num(go, "current");
		const Maybe<double> target = Num(go, "target");
		/* BOTH OR NEITHER. A target with no current cannot be drawn as a meter and a current with no
		   target divides by a number nobody sent; the rig writes them together (it clamps
		   target to >= 1 on every write) so a half-present goal is a reply that has gone weird. */
		if (current && target && *target > 0.0) {
			Goal g;
			g.current = *current;
			g.target = *target;
			g.label = PlainStr(go, "label");
			q.goal = g;
		}
	}

	const QJsonValue share = o.value(QLatin1String("share"));
	if (share.isObject()) {
		const Maybe<QString> url = Str(share.toObject(), "url");
		if (url && !url->isEmpty()) {
			q.share = *url;
		}
	}
	/* `share: null` is the idle card and lands here as an absent optional — which is what the TANK
	   page draws as "IDLE", never as an empty URL box that looks like a box nobody has filled in. */

	return q;
}

/* ── GET /obs ────────────────────────────────────────────────────────────────────────────────────
 * ONE BIT IS READ OUT OF THIS ROUTE AND THE REST IS DELIBERATELY IGNORED. /obs also carries the
 * scene, the mask, the stream stats and the whole mixer — every one of which is on /hub, arriving in
 * ONE instant with the mask and the hold. Reading them from here as well would give the panel two
 * ideas of the safety state on two different cadences, which is precisely the composite-of-four-
 * instants failure lib/hub.mjs spends its header arguing against. So: mirror, and nothing else. */
Obs ParseObs(const QJsonObject &o)
{
	Obs obs;
	obs.error = PlainStr(o, "error");
	if (!PlainBool(o, "ok")) {
		/* ok:false IS NOT A 5xx on this rig — "a down OBS is a normal, expected state" — so this is
		   the ordinary path, not an error path, and it leaves `mirror` absent, which is the third
		   state the switch renders as NO ANSWER. */
		return obs;
	}
	obs.ok = true;
	/* `mirror` is true | false | null and the null is load-bearing. Bool() answers std::nullopt for
	   a JSON null, which is exactly the reading wanted.
	 *
	 * ⚠️ `present` AND `mixed` ARE NOT ON THIS ROUTE — they are on GET /mirror's own bare reply,
	 * which walks every camera and costs a round of GetSourceFilterList per angle. /obs publishes
	 * the DERIVED bit only (deriveMirror(): true when every camera carrying the filter has it on,
	 * null when they disagree or it is unreadable). So the fork reads the one bit and leaves the two
	 * fields below absent rather than parsing keys this route has never sent — an optional that is
	 * always empty because nobody sends it looks exactly like a rig that has stopped answering. */
	obs.mirror = Bool(o, "mirror");
	return obs;
}

/* ── GET /reactions ──────────────────────────────────────────────────────────────────────────── */

Reactions ParseReactions(const QJsonObject &o)
{
	Reactions r;
	if (!PlainBool(o, "ok") || !o.value(QLatin1String("items")).isArray()) {
		return r;
	}
	r.ok = true;

	const QJsonArray items = o.value(QLatin1String("items")).toArray();
	for (const auto v : items) {
		if (!v.isObject()) {
			continue;
		}
		const QJsonObject io = v.toObject();
		Reaction it;
		it.id = PlainStr(io, "id");
		if (it.id.isEmpty()) {
			continue;
		}
		it.title = PlainStr(io, "title");
		it.status = PlainStr(io, "status");
		r.items.append(it);
	}

	const QJsonValue up = o.value(QLatin1String("upNext"));
	r.upNext = up.isArray() ? static_cast<int>(up.toArray().size()) : 0;

	const QJsonValue cur = o.value(QLatin1String("current"));
	if (cur.isObject()) {
		const QJsonObject co = cur.toObject();
		const QString id = PlainStr(co, "id");
		if (!id.isEmpty()) {
			r.currentId = id;
		}
		r.currentTitle = PlainStr(co, "title");
		const QJsonValue tv = co.value(QLatin1String("transport"));
		if (tv.isObject()) {
			const QJsonObject to = tv.toObject();
			r.control = PlainBool(to, "control");
			r.reason = PlainStr(to, "reason");
		}
	}

	const QJsonValue rep = o.value(QLatin1String("report"));
	if (rep.isObject()) {
		const QJsonObject ro = rep.toObject();
		r.state = PlainStr(ro, "state");
		/* `t` IS DELIBERATELY ABSENT-OR-NULL AND NOT ZERO. "YouTube volunteers currentTime over an
		   undocumented channel; when it is absent the server reports t:null and these two grey out
		   rather than seeking from a number nobody measured. A relative seek computed from a
		   position we do not have is a scrub bar that lies." */
		r.position = Num(ro, "t");
	}

	const Maybe<double> rate = Num(o, "rate");
	r.rate = rate ? *rate : 1.0;

	const QJsonValue rates = o.value(QLatin1String("rates"));
	if (rates.isArray()) {
		const QJsonArray arr = rates.toArray();
		for (const auto v : arr) {
			if (v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() > 0) {
				r.rates.append(v.toDouble());
			}
		}
	}

	return r;
}

/* ── GET /chat ───────────────────────────────────────────────────────────────────────────────── */

Chat ParseChat(const QJsonObject &o)
{
	Chat c;
	c.reason = PlainStr(o, "reason");
	if (!PlainBool(o, "ok")) {
		return c;
	}
	const Maybe<QString> url = Str(o, "url");
	if (!url || url->isEmpty()) {
		return c;
	}
	c.ok = true;
	c.url = *url;
	c.source = PlainStr(o, "source");
	return c;
}

/* ── GET /audio-routing ──────────────────────────────────────────────────────────────────────── */

Audio ParseAudio(const QJsonObject &o)
{
	Audio a;
	if (!PlainBool(o, "ok") || !o.value(QLatin1String("summary")).isObject()) {
		return a;
	}
	a.ok = true;
	a.live = PlainBool(o, "live");

	const QJsonObject s = o.value(QLatin1String("summary")).toObject();
	a.monitor = PlainStr(s, "monitor");
	a.addedLatencyMs = PlainInt(s, "addedLatencyMs");
	a.slackMs = PlainInt(s, "slackMs");

	const QJsonValue sources = s.value(QLatin1String("sources"));
	if (sources.isArray()) {
		const QJsonArray arr = sources.toArray();
		for (const auto v : arr) {
			if (!v.isObject()) {
				continue;
			}
			const QJsonObject so = v.toObject();
			Channel ch;
			ch.name = PlainStr(so, "name");
			if (ch.name.isEmpty()) {
				continue;
			}
			ch.from = PlainStr(so, "from");
			const QJsonValue to = so.value(QLatin1String("to"));
			if (to.isArray()) {
				const QJsonArray dests = to.toArray();
				for (const auto d : dests) {
					if (d.isString()) {
						ch.to.append(d.toString());
					}
				}
			}
			ch.alignMs = PlainInt(so, "alignMs");
			ch.muted = PlainBool(so, "muted");
			ch.tone = PlainStr(so, "tone");
			ch.note = PlainStr(so, "note");
			a.sources.append(ch);
		}
	}

	const QJsonValue taps = s.value(QLatin1String("taps"));
	if (taps.isArray()) {
		const QJsonArray arr = taps.toArray();
		for (const auto v : arr) {
			if (!v.isObject()) {
				continue;
			}
			const QJsonObject to = v.toObject();
			Tap t;
			t.deviceName = PlainStr(to, "deviceName");
			t.input = PlainStr(to, "input");
			t.ok = PlainBool(to, "ok");
			t.verified = PlainBool(to, "verified");
			t.label = PlainStr(to, "label");
			t.tone = PlainStr(to, "tone");
			a.taps.append(t);
		}
	}

	/* COUNTED HERE, from the violation list, rather than trusted from a summary field — the two
	   severities mean different things to the operator and the rig keeps them apart deliberately:
	   "a warn — a bus with no writer — is a real finding and belongs on the big screen, but a count
	   that is never zero stops being read." */
	const QJsonValue violations = o.value(QLatin1String("violations"));
	if (violations.isArray()) {
		const QJsonArray arr = violations.toArray();
		for (const auto v : arr) {
			if (!v.isObject()) {
				continue;
			}
			const QString sev = PlainStr(v.toObject(), "severity");
			if (sev == QLatin1String("block")) {
				a.blocking++;
			} else if (sev == QLatin1String("warn")) {
				a.warnings++;
			}
		}
	}

	return a;
}

/* ── formatting, once ────────────────────────────────────────────────────────────────────────── */

QString FormatDuration(qint64 ms)
{
	if (ms < 0) {
		ms = 0;
	}
	const qint64 total = ms / 1000;
	const qint64 h = total / 3600;
	const qint64 m = (total % 3600) / 60;
	const qint64 s = total % 60;
	return QStringLiteral("%1:%2:%3")
		.arg(h)
		.arg(m, 2, 10, QLatin1Char('0'))
		.arg(s, 2, 10, QLatin1Char('0'));
}

QString FormatAmount(const Maybe<double> &amount, const QString &currency)
{
	/* EMPTY, NOT "$0". A follow, a raid and a chat message all arrive on this queue with no money
	   attached; printing a zero dollar figure on one of those cards invents a donation that did not
	   happen, on the surface the operator reads aloud from. */
	if (!amount || *amount == 0.0) {
		return QString();
	}
	const QString sym = currency.isEmpty() ? QStringLiteral("USD") : currency.toUpper();
	return QStringLiteral("%1 %2").arg(QString::number(*amount, 'f', 2), sym);
}

QString FormatAge(qint64 ms)
{
	if (ms < 0) {
		ms = 0;
	}
	if (ms < 1000) {
		return QStringLiteral("NOW");
	}
	if (ms < 60000) {
		return QStringLiteral("%1S AGO").arg(ms / 1000);
	}
	return QStringLiteral("%1M AGO").arg(ms / 60000);
}

QString FormatMoney(double amount)
{
	/* WHOLE DOLLARS, like the console's money0(). The goal is read across a room in a glance and
	   cents on a fundraising figure are noise; the queue card's FormatAmount() keeps them because a
	   donation is a specific sum somebody sent. Two formats, two jobs, one place each. */
	return QStringLiteral("$%1").arg(QString::number(qRound64(amount)));
}

} // namespace QCiRig
