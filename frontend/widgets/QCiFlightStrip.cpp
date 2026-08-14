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

#include "QCiFlightStrip.hpp"

#include <docks/QCiRigClient.hpp>
#include <docks/QCiRigUi.hpp>

#include <obs.hpp>
/* obs_frontend_recording_active() — the recording lamp is this process's own fact, read straight
   out of the frontend API rather than off a rig route. Recording and streaming are different states
   and never share an indicator: the whole YouTube close-out alarm exists because a recording that
   is quietly public is this rig's worst outcome. */
#include <obs-frontend-api.h>

#include <QButtonGroup>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include "moc_QCiFlightStrip.cpp"

using namespace QCiRig;
using QCiUi::ABSENT;
using QCiUi::ApplyLabelFont;
using QCiUi::ApplyReadoutFont;
using QCiUi::MakeButton;
using QCiUi::MakeLabel;
using QCiUi::MakeSeparator;
using QCiUi::SetTone;

namespace {

/* SPEC widths, in the layout spec's own numbers. They are fixed rather than computed because this
   strip must not reflow when a value changes width — a readout that moves is a readout the eye has
   to re-find, and the whole point of the band is that a glance lands in the same place every time. */
constexpr int STRIP_HEIGHT = 48;
constexpr int AIR_W = 168;
constexpr int CLOCK_W = 96;
constexpr int SCENE_W = 220;
constexpr int CHIP_W = 76;
constexpr int CHIP_H = 38;
constexpr int SMALL_CHIP_H = 26;
constexpr int LAMP_MS = 850;

} // namespace

QCiFlightStrip::QCiFlightStrip(QWidget *parent) : QWidget(parent)
{
	/* Without this the theme does not reach a bare QWidget subclass at all, and the failure is
	   silent: QCi.obt's rule for #qciFlightStrip parses, resolves, matches, and paints nothing. */
	setAttribute(Qt::WA_StyledBackground, true);
	setObjectName(QStringLiteral("qciFlightStrip"));
	setFixedHeight(STRIP_HEIGHT);

	QHBoxLayout *row = new QHBoxLayout(this);
	row->setContentsMargins(12, 4, 12, 4);
	row->setSpacing(16);

	/* ── 1. ON AIR / OFF AIR ────────────────────────────────────────────────────────────────────
	 * The largest single-colour object in the chrome, at the top-left where the eye lands first.
	 * Off air it is an outlined plate; on air it INVERTS to a solid amber field. See the note on
	 * m_blink in the header for why the field is steady and only the 10px lamp moves. */
	m_air = new QWidget;
	m_air->setObjectName(QStringLiteral("qciFsAir"));
	m_air->setAttribute(Qt::WA_StyledBackground, true);
	m_air->setFixedSize(AIR_W, 40);
	{
		QHBoxLayout *air = new QHBoxLayout(m_air);
		air->setContentsMargins(8, 0, 8, 0);
		air->setSpacing(8);
		m_airLamp = MakeLabel(QString(), "qciFsAirLamp");
		m_airLamp->setFixedSize(10, 10);
		air->addWidget(m_airLamp);
		m_airText = MakeLabel(QStringLiteral("OFF AIR"), "qciFsAirText");
		ApplyLabelFont(m_airText, 112.0, true);
		air->addWidget(m_airText, 1);
		/* RECORDING IS NOT STREAMING AND THEY NEVER SHARE AN INDICATOR. The whole YouTube
		   close-out alarm exists because a recording that is quietly public is this rig's worst
		   outcome, so a local recording gets its own 6px square at the right edge of this block
		   and nothing else in the window changes. */
		m_recDot = MakeLabel(QString(), "qciFsRecDot");
		m_recDot->setFixedSize(6, 6);
		m_recDot->setVisible(false);
		air->addWidget(m_recDot);
	}
	row->addWidget(m_air);

	/* ── 2. the uptime clock ────────────────────────────────────────────────────────────────── */
	m_clock = MakeLabel(QString::fromUtf8(ABSENT), "qciFsClock");
	m_clock->setFixedSize(CLOCK_W, 40);
	m_clock->setAlignment(Qt::AlignCenter);
	ApplyReadoutFont(m_clock, 20);
	m_clock->setToolTip(QStringLiteral("Stream uptime, as the events server computes it (/hub sinceMs). "
					   "Never a start time this application remembered."));
	row->addWidget(m_clock);

	row->addWidget(MakeSeparator(28));

	/* ── 4. the program scene ───────────────────────────────────────────────────────────────────
	 * ⚠️ ELIDED, NEVER WRAPPED AND NEVER RESIZED. A scene name that widens this label pushes every
	 * chip to its right along the strip, and the operator's glance would land on a different thing
	 * depending on how long the scene name happens to be. */
	m_scene = MakeLabel(QString::fromUtf8(ABSENT), "qciFsScene");
	m_scene->setFixedSize(SCENE_W, 40);
	m_scene->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
	ApplyLabelFont(m_scene, 108.0, true);
	row->addWidget(m_scene);

	/* ── 5. the mask chip. BLOCK is the louder of the two on purpose. ───────────────────────── */
	m_mask = MakeLabel(QString::fromUtf8(ABSENT), "qciFsMask");
	m_mask->setFixedSize(84, SMALL_CHIP_H);
	m_mask->setAlignment(Qt::AlignCenter);
	ApplyLabelFont(m_mask, 110.0, true);
	row->addWidget(m_mask);

	/* ── 6. the watchdog readout ────────────────────────────────────────────────────────────── */
	m_watchdog = MakeLabel(QStringLiteral("WCHDG %1").arg(QString::fromUtf8(ABSENT)), "qciFsWatchdog");
	m_watchdog->setFixedSize(132, SMALL_CHIP_H);
	m_watchdog->setAlignment(Qt::AlignCenter);
	ApplyLabelFont(m_watchdog, 110.0);
	/* ⚠️ SAID OUT LOUD, IN THE TOOLTIP, BECAUSE IT IS AN INFERENCE. There is no watchdog feed on
	   any route this fork reads. What /hub publishes is `sceneOk` — whether the CURRENT program
	   scene is in the allowlist — and OK here means "the scene on program is one the watchdog
	   permits", not "the watchdog process is alive and well". Claiming the second from the first
	   would be exactly the kind of green-over-a-dead-link this rig refuses. */
	m_watchdog->setToolTip(QStringLiteral("INFERRED, not a watchdog feed: OK means /hub says the program "
					      "scene is in the allowlist (sceneOk). It does not prove the watchdog "
					      "process is running."));
	row->addWidget(m_watchdog);

	row->addStretch(1);

	/* ── 8. the five vitals ─────────────────────────────────────────────────────────────────── */
	{
		QWidget *vitals = new QWidget;
		vitals->setObjectName(QStringLiteral("qciFsVitals"));
		QHBoxLayout *v = new QHBoxLayout(vitals);
		v->setContentsMargins(0, 0, 0, 0);
		v->setSpacing(4);
		v->addWidget(makeChip("qciFsChipBr", QStringLiteral("BR"), &m_br, CHIP_W));
		v->addWidget(makeChip("qciFsChipFps", QStringLiteral("FPS"), &m_fps, CHIP_W));
		v->addWidget(makeChip("qciFsChipDrop", QStringLiteral("DROP"), &m_drop, CHIP_W));
		v->addWidget(makeChip("qciFsChipLag", QStringLiteral("LAG"), &m_lag, CHIP_W));
		v->addWidget(makeChip("qciFsChipCpu", QStringLiteral("CPU"), &m_cpu, CHIP_W));
		row->addWidget(vitals);
	}

	/* ⚠️ FOUR OF THE FIVE ARE THE SERVER'S NUMBERS AND ONE IS THIS PROCESS'S, AND THAT SPLIT IS
	   DELIBERATE. BR / FPS / DROP / CPU come off /hub so the app, the 4" Hub and the 7" console
	   cannot disagree about the same encoder — dropPct in particular is derived once, in
	   lib/hub.mjs, precisely so two surfaces cannot round it differently and argue.
	   LAG has no server copy at all; it is obs_get_lagged_frames(), read out of the libobs this
	   binary IS. There is nothing for it to disagree with, and inventing a route to fetch a number
	   that is already in-process would be the same duplication in the other direction. */
	m_drop->setToolTip(QStringLiteral("Dropped frames %, from the events server (lib/hub.mjs)."));
	m_lag->setToolTip(QStringLiteral("Lagged frames, from this process's own libobs "
					 "(obs_get_lagged_frames). Not a rig number."));

	row->addWidget(MakeSeparator(28));

	/* ── 10-12. moderation and money ────────────────────────────────────────────────────────── */
	row->addWidget(makeChip("qciFsQ", QStringLiteral("Q"), &m_q, 56));
	row->addWidget(makeChip("qciFsA", QStringLiteral("A"), &m_a, 56));
	row->addWidget(makeChip("qciFsGoal", QStringLiteral("GOAL"), &m_goal, 116));
	/* ⚠️ THE ONE PLACE IN THIS BAND THAT IS ALLOWED TO BE PINK, because it is money. The rest of
	   the strip is green chrome, cyan numerals and amber for on-air; pink's entire value is
	   scarcity, and a hue spent on a permanently-visible indicator signals nothing. The theme is
	   what makes #qciFsGoal pink — this file names no colour.
	   Formatted through FormatMoney() rather than here, so the three widgets that draw a goal
	   figure cannot round or punctuate it three ways. */
	m_goal->setToolTip(QStringLiteral("Donation goal, current of target, from GET /queue. Absent when the "
					  "rig has published no goal — never drawn as $0."));

	/* ── 13. the VOD alarm ──────────────────────────────────────────────────────────────────────
	   Hidden unless there is an alarm, and it lives HERE and nowhere else because an alarm only
	   visible inside a dialog is not an alarm. Same honesty as the goal: no route this fork reads
	   publishes `alarm`, so this chip is constructed, wired to a setter, and currently never shown.
	   A chip that COULD light is worth its 148px; a chip that lies is not. */
	m_vod = MakeLabel(QStringLiteral("VOD ALARM"), "qciFsVod");
	m_vod->setFixedSize(148, SMALL_CHIP_H);
	static_cast<QLabel *>(m_vod)->setAlignment(Qt::AlignCenter);
	ApplyLabelFont(m_vod, 110.0, true);
	SetTone(m_vod, QStringLiteral("bad"));
	m_vod->setVisible(false);
	row->addWidget(m_vod);

	/* ── 14. the health lamps ───────────────────────────────────────────────────────────────────
	 * ⚠️ EVT AND OBS ARE TWO LAMPS AND STAY TWO LAMPS FOREVER. The events server answers /hub with
	 * OBS dead — it makes no OBS call to do it — so "the rig is up" and "OBS is up" are genuinely
	 * independent facts, and collapsing them destroys the one honest health signal on the rig: a
	 * green EVT beside a red OBS is the exact picture of "my panel works, my encoder is gone".
	 *
	 * The spec's third lamp, PHONE, is NOT built. Nothing on /hub, /queue, /reactions, /chat or
	 * /audio-routing reports the phone rig; a lamp with no feed would be either permanently dark
	 * (indistinguishable from "phone is down") or permanently green (a lie). Two honest lamps beat
	 * three where one is decoration. */
	{
		QWidget *lamps = new QWidget;
		lamps->setObjectName(QStringLiteral("qciFsLamps"));
		QHBoxLayout *l = new QHBoxLayout(lamps);
		l->setContentsMargins(0, 0, 0, 0);
		l->setSpacing(8);
		m_lampEvt = makeLamp("qciFsLampEvt", QStringLiteral("EVT"));
		m_lampObs = makeLamp("qciFsLampObs", QStringLiteral("OBS"));
		l->addWidget(m_lampEvt->parentWidget());
		l->addWidget(m_lampObs->parentWidget());
		row->addWidget(lamps);
	}

	row->addWidget(MakeSeparator(28));

	/* ── the RUN | BUILD segment ────────────────────────────────────────────────────────────────
	 * Deliberately two QPushButtons in an exclusive group and NOT a QTabBar. A QTabBar is the single
	 * most recognisably-OBS piece of chrome there is, and the one structural idea of this window is
	 * that the editor is a MODE you summon rather than the thing you live in. */
	{
		QWidget *seg = new QWidget;
		seg->setObjectName(QStringLiteral("qciFsMode"));
		QHBoxLayout *s = new QHBoxLayout(seg);
		s->setContentsMargins(0, 0, 0, 0);
		s->setSpacing(0);
		m_run = MakeButton(QStringLiteral("RUN"), "qciFsModeRun");
		m_build = MakeButton(QStringLiteral("BUILD"), "qciFsModeBuild");
		QButtonGroup *group = new QButtonGroup(this);
		group->setExclusive(true);
		for (QPushButton *b : {m_run, m_build}) {
			b->setCheckable(true);
			b->setFixedSize(48, 28);
			ApplyLabelFont(b, 110.0, true);
			group->addButton(b);
			s->addWidget(b);
		}
		m_run->setChecked(true);
		/* ⚠️ THE SEGMENT ASKS; IT DOES NOT DECIDE, AND IT DOES NOT SET ITS OWN CHECK STATE.
		   OBSBasic owns the mode, and it answers by calling setBuildMode(). If the two were wired
		   directly the segment could sit on BUILD while the window was still in RUN — which is the
		   class of bug where a control's drawn state and the thing it controls disagree. */
		connect(m_run, &QPushButton::clicked, this, [this]() { emit modeRequested(false); });
		connect(m_build, &QPushButton::clicked, this, [this]() { emit modeRequested(true); });
		row->addWidget(seg);
	}

	/* THE ONE SUBSCRIPTION. Two surfaces: /hub for everything safety-shaped, and /queue for the two
	   moderation counters. Declaring /queue here means it is polled while the window is open, which
	   is correct — the counters are chrome and chrome is never paged away. */
	QCiRigClient *client = QCiRigClient::Get();
	client->wantSurface(QCiRigClient::SurfaceHub, this);
	client->wantSurface(QCiRigClient::SurfaceQueue, this);
	connect(client, &QCiRigClient::changed, this, &QCiFlightStrip::onModelChanged);

	m_blink = new QTimer(this);
	m_blink->setInterval(LAMP_MS);
	connect(m_blink, &QTimer::timeout, this, [this]() {
		m_blinkOn = !m_blinkOn;
		SetTone(m_airLamp, m_blinkOn ? QStringLiteral("live") : QString());
	});

	renderModel(client->model());
}

QWidget *QCiFlightStrip::makeChip(const char *objectName, const QString &legend, QLabel **valueOut, int width)
{
	QWidget *chip = new QWidget;
	chip->setObjectName(QString::fromUtf8(objectName));
	chip->setAttribute(Qt::WA_StyledBackground, true);
	chip->setFixedSize(width, CHIP_H);

	QVBoxLayout *col = new QVBoxLayout(chip);
	col->setContentsMargins(4, 2, 4, 2);
	col->setSpacing(0);

	QLabel *key = MakeLabel(legend, "qciFsChipKey");
	key->setAlignment(Qt::AlignCenter);
	ApplyLabelFont(key, 120.0);
	col->addWidget(key);

	QLabel *value = MakeLabel(QString::fromUtf8(ABSENT), "qciFsChipValue");
	value->setAlignment(Qt::AlignCenter);
	ApplyReadoutFont(value, 13);
	col->addWidget(value);

	*valueOut = value;
	return chip;
}

QLabel *QCiFlightStrip::makeLamp(const char *objectName, const QString &legend)
{
	QWidget *box = new QWidget;
	QVBoxLayout *col = new QVBoxLayout(box);
	col->setContentsMargins(0, 0, 0, 0);
	col->setSpacing(2);
	col->setAlignment(Qt::AlignCenter);

	QLabel *lamp = MakeLabel(QString(), objectName);
	lamp->setFixedSize(10, 10);
	col->addWidget(lamp, 0, Qt::AlignHCenter);

	QLabel *key = MakeLabel(legend, "qciFsLampKey");
	key->setAlignment(Qt::AlignCenter);
	ApplyLabelFont(key, 120.0);
	col->addWidget(key);

	return lamp;
}

void QCiFlightStrip::setLamp(QLabel *lamp, const QString &tone)
{
	SetTone(lamp, tone);
}

void QCiFlightStrip::setBuildMode(bool build)
{
	QSignalBlocker blockRun(m_run);
	QSignalBlocker blockBuild(m_build);
	m_run->setChecked(!build);
	m_build->setChecked(build);
}

void QCiFlightStrip::onModelChanged()
{
	renderModel(QCiRigClient::Get()->model());
}

void QCiFlightStrip::renderModel(const Model &model)
{
	const Hub &h = model.hub;
	/* THREE STATES, NEVER TWO. `ever` false is "nobody has asked the rig anything yet"; a fault on
	   top of a filled block is "this is the last thing it said, and that was a while ago". Neither
	   is the same as a fact. */
	const bool fresh = h.fresh.ever && h.fresh.fault.isEmpty();

	/* ── on air ─────────────────────────────────────────────────────────────────────────────────
	   `live` is a Maybe<bool> with three states and the third one is not `false`: it means nobody
	   has been able to ask OBS. That draws as ABSENT, not as OFF AIR — "OFF AIR" over a dead link
	   is the single most dangerous string this band could print. */
	const bool liveNow = fresh && h.live && *h.live;
	if (liveNow != m_live) {
		m_live = liveNow;
		if (liveNow) {
			m_blinkOn = true;
			SetTone(m_airLamp, QStringLiteral("live"));
			m_blink->start();
		} else {
			m_blink->stop();
			SetTone(m_airLamp, QString());
		}
	}
	if (!h.live || !h.fresh.ever) {
		m_airText->setText(QStringLiteral("AIR %1").arg(QString::fromUtf8(ABSENT)));
		SetTone(m_air, QStringLiteral("bad"));
	} else if (*h.live) {
		m_airText->setText(QStringLiteral("ON AIR"));
		SetTone(m_air, QStringLiteral("live"));
	} else {
		m_airText->setText(QStringLiteral("OFF AIR"));
		SetTone(m_air, fresh ? QString() : QStringLiteral("warn"));
	}

	/* ⚠️ THE WHOLE STRIP CARRIES THE STATE, NOT JUST THE BLOCK. This property is what the theme
	   hangs the full-width amber waterline on — a 1920px line across the top of the screen that is
	   visible in peripheral vision and in a monitor reflection, with the operator's eyes on the
	   camera rather than on the screen. */
	SetTone(this, liveNow ? QStringLiteral("live") : QString());

	/* Recording is read from libobs directly. It is this process's own fact and there is no rig
	   copy of it to disagree with. */
	m_recDot->setVisible(obs_frontend_recording_active());

	m_clock->setText(h.sinceMs ? FormatDuration(*h.sinceMs) : QString::fromUtf8(ABSENT));
	SetTone(m_clock, liveNow ? QStringLiteral("live") : QString());

	/* ── program scene ──────────────────────────────────────────────────────────────────────────
	   The label is elided to the widget rather than the widget grown to the label. */
	if (!h.scene) {
		m_scene->setText(QString::fromUtf8(ABSENT));
		SetTone(m_scene, QStringLiteral("bad"));
	} else {
		const QString shown = QCiUi::SceneLabelFor(*h.scene);
		m_scene->setText(m_scene->fontMetrics().elidedText(shown, Qt::ElideRight, m_scene->width() - 8));
		m_scene->setToolTip(*h.scene);
		SetTone(m_scene, (h.sceneOk && !*h.sceneOk) ? QStringLiteral("bad") : QString());
	}

	/* ── mask ───────────────────────────────────────────────────────────────────────────────────
	   Absent is drawn as absent. Stale is drawn as unverified. Neither is drawn as a mode: a mask
	   chip is a claim about whether the operator's face is on a broadcast, and it may report what
	   was last seen but it may not assert it. */
	if (!h.mask) {
		m_mask->setText(QString::fromUtf8(ABSENT));
		SetTone(m_mask, QStringLiteral("bad"));
	} else if (!fresh) {
		m_mask->setText(QStringLiteral("? %1").arg(h.mask->toUpper()));
		SetTone(m_mask, QStringLiteral("warn"));
	} else {
		const bool block = *h.mask == QLatin1String("block");
		m_mask->setText(block ? QStringLiteral("BLOCK") : QStringLiteral("VISION"));
		SetTone(m_mask, block ? QStringLiteral("live") : QStringLiteral("go"));
	}

	/* ── watchdog, inferred ─────────────────────────────────────────────────────────────────── */
	if (!h.obsAgeMs) {
		m_watchdog->setText(QStringLiteral("OBS DOWN"));
		SetTone(m_watchdog, QStringLiteral("bad"));
	} else if (h.sceneOk && !*h.sceneOk) {
		m_watchdog->setText(QStringLiteral("WCHDG FAULT"));
		SetTone(m_watchdog, QStringLiteral("bad"));
	} else if (!fresh) {
		m_watchdog->setText(QStringLiteral("WCHDG %1").arg(QString::fromUtf8(ABSENT)));
		SetTone(m_watchdog, QStringLiteral("warn"));
	} else {
		m_watchdog->setText(QStringLiteral("WCHDG OK"));
		SetTone(m_watchdog, QStringLiteral("go"));
	}

	/* ── vitals ─────────────────────────────────────────────────────────────────────────────────
	   ⚠️ THE RIG PUBLISHES THE ON-AIR NUMBERS ONLY WHILE ON AIR, deliberately: "kbps: 0 while LIVE
	   is an encoder that has stopped sending, which the operator has seconds to notice." So an
	   absent number is drawn absent and never as a zero. */
	m_br->setText(h.kbps ? QString::number(*h.kbps, 'f', 0) : QString::fromUtf8(ABSENT));
	m_fps->setText(h.fps ? QString::number(*h.fps, 'f', 1) : QString::fromUtf8(ABSENT));
	m_drop->setText(h.droppedPct ? QString::number(*h.droppedPct, 'f', 2) : QString::fromUtf8(ABSENT));
	m_cpu->setText(h.cpu ? QString::number(*h.cpu, 'f', 1) : QString::fromUtf8(ABSENT));
	SetTone(m_drop, (h.droppedPct && *h.droppedPct >= 1.0) ? QStringLiteral("warn") : QString());

	const uint32_t lagged = obs_get_lagged_frames();
	m_lag->setText(QString::number(lagged));
	SetTone(m_lag, lagged > 0 ? QStringLiteral("warn") : QString());

	/* ── moderation counters ────────────────────────────────────────────────────────────────────
	   An unanswered /queue is ABSENT, not 0. "Nothing waiting" and "nobody has told me" look the
	   same in a counter and mean opposite things about whether there is work to do. */
	const Queue &q = model.queue;
	const bool queueFresh = q.fresh.ever && q.fresh.fault.isEmpty();
	m_q->setText(queueFresh ? QString::number(q.pending.size()) : QString::fromUtf8(ABSENT));
	m_a->setText(queueFresh ? QString::number(q.approved.size()) : QString::fromUtf8(ABSENT));
	SetTone(m_q, (queueFresh && !q.pending.isEmpty()) ? QStringLiteral("warn") : QString());

	/* ⚠️ ABSENT, NEVER $0. A goal figure reading zero is a claim that nobody has given anything,
	   which is a different statement from "the rig has not published a goal" — and the second one
	   is what an unanswered /queue means. The Maybe<Goal> is the model refusing to default it, and
	   this branch is that refusal reaching the glass. */
	if (!queueFresh || !q.goal) {
		m_goal->setText(QString::fromUtf8(ABSENT));
	} else {
		m_goal->setText(QStringLiteral("%1/%2").arg(FormatMoney(q.goal->current), FormatMoney(q.goal->target)));
	}

	/* ── health lamps ───────────────────────────────────────────────────────────────────────────
	   EVT is "did the events server answer THIS fork", which is the /hub block's own freshness.
	   OBS is "how old is the events server's mirror of OBS", which is a different question with a
	   different answer, and the pair is the whole diagnostic. */
	if (!h.fresh.ever) {
		setLamp(m_lampEvt, QStringLiteral("bad"));
	} else {
		setLamp(m_lampEvt, fresh ? QStringLiteral("go") : QStringLiteral("warn"));
	}
	if (!h.obsAgeMs) {
		setLamp(m_lampObs, QStringLiteral("bad"));
	} else {
		setLamp(m_lampObs, *h.obsAgeMs > 15000 ? QStringLiteral("warn") : QStringLiteral("go"));
	}
}
