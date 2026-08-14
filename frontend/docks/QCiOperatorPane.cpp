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

#include "QCiOperatorPane.hpp"

#include "QCiRigClient.hpp"
#include "QCiRigPanes.hpp"
#include "QCiRigUi.hpp"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "moc_QCiOperatorPane.cpp"

using namespace QCiRig;
using QCiUi::ApplyLabelFont;
using QCiUi::ApplyReadoutFont;
using QCiUi::MakeButton;
using QCiUi::MakeLabel;
using QCiUi::SetTone;

namespace {

/* ── THE SPACING SCALE. Base unit 4; these are the only values this file may use. ──────────────── */
constexpr int PANE_PAD = 12;
constexpr int ROW_GAP = 8;
constexpr int TIGHT_GAP = 4;

constexpr int PANIC_H = 56;
constexpr int MASK_H = 40;
constexpr int TOGGLE_H = 36;
constexpr int PAGER_H = 34;
constexpr int STREAM_H = 44;
constexpr int ARM_H = 30;

/* HOW LONG THE PANIC IS WATCHED FOR. The watchdog's own tick is 80ms and it also fires on eleven OBS
   events, so if it is going to undo a manual hold it does so almost immediately; /hub is polled at
   1Hz, so the window has to cover several polls to see the result at all. Six seconds is long enough
   to catch it and short enough that the alarm cannot be mistaken for a permanent state. */
constexpr qint64 HOLD_WATCH_MS = 6000;

} // namespace

QCiOperatorPane::QCiOperatorPane(QWidget *parent) : QWidget(parent)
{
	/* ⚠️ WITHOUT THIS THE THEME DOES NOT REACH THIS WIDGET AT ALL, and the failure is silent — Qt
	   honours a stylesheet background on a bare QWidget subclass only when the subclass reimplements
	   paintEvent() or carries WA_StyledBackground. QCi.obt's rule for this object name would parse,
	   resolve, match, and paint nothing. */
	setAttribute(Qt::WA_StyledBackground, true);
	setObjectName(QStringLiteral("qciOperatorPane"));

	QVBoxLayout *outer = new QVBoxLayout(this);
	outer->setContentsMargins(PANE_PAD, ROW_GAP, PANE_PAD, PANE_PAD);
	outer->setSpacing(ROW_GAP);

	outer->addWidget(buildPrivacyBlock());
	outer->addWidget(buildPager());
	outer->addWidget(m_stack, 1);
	outer->addWidget(buildTransport());

	QCiRigClient *client = QCiRigClient::Get();
	/* THE PANE'S OWN SUBSCRIPTIONS. /hub carries the safety block and is polled unconditionally;
	   /queue is declared here because BRB's state rides on it and BRB is in the PINNED block, which
	   is on screen whatever page the operator is on. /queue makes zero OBS calls, so depending on it
	   for a privacy control is safe in exactly the way depending on /obs would not be. /obs is
	   declared for the mirror's three-state answer and nothing else. */
	client->wantSurface(QCiRigClient::SurfaceHub, this);
	client->wantSurface(QCiRigClient::SurfaceQueue, this);
	client->wantSurface(QCiRigClient::SurfaceObs, this);
	connect(client, &QCiRigClient::changed, this, &QCiOperatorPane::onModelChanged);

	/* ⚠️ THIS IS WHERE A REFUSAL LANDS. Pinned to the foot, never in the paged middle: a refusal the
	   operator has to switch pages to find is a refusal they act as though never happened. It is fed
	   from the REPLY (QCiRigClient::say), never from the handler that queued the request — the bug
	   this shape fixes is /say printing "SAID" unconditionally over silence. */
	connect(client, &QCiRigClient::say, this, [this](const QString &text) {
		m_say->setText(text);
		SetTone(m_say, text.startsWith(QStringLiteral("REFUSED")) || text.contains(QStringLiteral("FAULT"))
					? QStringLiteral("bad")
					: QString());
	});

	/* ⚠️ THE PANIC'S THIRD STEP IS REPORTED SEPARATELY BECAUSE IT IS THE ONE OUTCOME THE OPERATOR
	   CANNOT INFER FROM ANYTHING ELSE ON THE GLASS. The virtual camera is PINNED in this collection
	   (type2:1 SceneOutput → "PANELS (Delayed Output)"), so it renders its pinned target regardless
	   of program: a scene switch does not take it off air at all, and only StopVirtualCam does. The
	   rig answers the hold with one of three words and they are three different rig states —
	   "stopped" (it was cut), "idle" (there was nothing to cut) and "unreachable" (NOBODY KNOWS).
	   Flattening those into the one-line readout would make the third look like the second. */
	connect(client, &QCiRigClient::holdAnswered, this,
		[this](bool ok, const QString &vcam, const QString &scene) {
			if (vcam == QLatin1String("stopped")) {
				m_vcam->setText(QStringLiteral("VCAM STOPPED"));
				SetTone(m_vcam, QStringLiteral("go"));
			} else if (vcam == QLatin1String("idle")) {
				/* NOT AN ALARM. cutVirtualCam() reads GetVirtualCamStatus first precisely so
				   that the common case — a hold pressed while the vcam happens to be off —
				   does not report a failed cut. Every alarm that cries on a normal day is one
				   the operator learns to ignore on the day it matters. */
				m_vcam->setText(QStringLiteral("VCAM ALREADY IDLE"));
				SetTone(m_vcam, QString());
			} else if (vcam == QLatin1String("unreachable")) {
				m_vcam->setText(QStringLiteral("VCAM UNREACHABLE — CUT NOT CONFIRMED"));
				SetTone(m_vcam, QStringLiteral("bad"));
			} else {
				/* No word at all: the reply was not one this client could read. Say that,
				   rather than nothing — silence here reads as "the cut was fine". */
				m_vcam->setText(ok ? QStringLiteral("VCAM — NO ANSWER IN THE REPLY")
						   : QStringLiteral("HOLD UNCONFIRMED — SEE THE LINE BELOW"));
				SetTone(m_vcam, QStringLiteral("bad"));
			}

			/* Arm the watch. See the header: the watchdog appears to push program back off the
			   hold scene within ~80ms of a MANUAL hold, and the operator has to be told if that
			   happens rather than left looking at a button they believe worked. */
			if (ok && !scene.isEmpty()) {
				m_holdScene = scene;
				m_holdWatchUntil = QCiRigClient::Get()->nowMs() + HOLD_WATCH_MS;
				m_holdAlarm->hide();
			}
		});

	renderModel(client->model());
}

/* ══ THE PINNED PRIVACY BLOCK ══════════════════════════════════════════════════════════════════ */

QWidget *QCiOperatorPane::buildPrivacyBlock()
{
	QWidget *block = new QWidget;
	block->setObjectName(QStringLiteral("qciOpPrivacy"));
	block->setAttribute(Qt::WA_StyledBackground, true);
	QVBoxLayout *col = new QVBoxLayout(block);
	col->setContentsMargins(ROW_GAP, ROW_GAP, ROW_GAP, ROW_GAP);
	col->setSpacing(ROW_GAP);

	/* ── PRIVACY HOLD ───────────────────────────────────────────────────────────────────────────
	 * ONE CLICK: no confirmation dialog, no "are you sure", no second request. qci-rig's hubAction()
	 * says why — "this is a panic control on a physical object beside the operator's face; a confirm
	 * token or a nonce is one more chance for the network to be the reason a camera stayed live."
	 *
	 * ⚠️ AND IT CALLS actHold(), NOT actScene(hold). The console's HOLD button, its H key and its
	 * `hold` verb all switch the scene and NOTHING ELSE, and that is the one thing this port must
	 * not inherit: the virtual camera is pinned to its own scene, so it keeps rendering that scene
	 * regardless of program and a scene switch does not take it off air at all. POST /hub/action?a=
	 * hold does three things in a fixed order — switch the scene first and fire-and-forget so it is
	 * in flight even if the socket hangs, then the mask block (one operation, because a panic that
	 * is two requests is a panic whose second half can be the one that never arrives), then await
	 * the vcam cut on a 2000ms budget. actHold() asks for all three as one decision and composes
	 * none of it here. */
	m_panic = MakeButton(QStringLiteral("PRIVACY HOLD"), "qciRigPanic");
	m_panic->setMinimumHeight(PANIC_H);
	m_panic->setToolTip(QStringLiteral("Switch program to the hold scene, block the camera mask, and cut "
					   "the virtual camera. One press, one decision, no confirmation."));
	ApplyLabelFont(m_panic, 140.0, true);
	connect(m_panic, &QPushButton::clicked, this, []() { QCiRigClient::Get()->actHold(); });
	col->addWidget(m_panic);

	m_privacy = MakeLabel(QString(), "qciRigPrivacy");
	m_privacy->setAlignment(Qt::AlignCenter);
	m_privacy->setWordWrap(true);
	ApplyLabelFont(m_privacy, 118.0, true);
	col->addWidget(m_privacy);

	/* The vcam outcome sits directly under the button that produced it, because it is the answer to
	   "did the hold actually take the camera off air" and that question is about this button. */
	m_vcam = MakeLabel(QString(), "qciRigVcam");
	m_vcam->setAlignment(Qt::AlignCenter);
	m_vcam->setWordWrap(true);
	ApplyLabelFont(m_vcam, 116.0);
	m_vcam->hide();
	col->addWidget(m_vcam);

	/* THE HOLD-DID-NOT-STICK ALARM. Hidden until it has something to say; see the header for the
	   watchdog branch it exists to catch. */
	m_holdAlarm = MakeLabel(QString(), "qciRigHoldAlarm");
	m_holdAlarm->setAlignment(Qt::AlignCenter);
	m_holdAlarm->setWordWrap(true);
	ApplyLabelFont(m_holdAlarm, 118.0, true);
	m_holdAlarm->hide();
	col->addWidget(m_holdAlarm);

	/* ── mask: TWO FIXED-DIRECTION ACTIONS, NOT A TOGGLE, EVER ──────────────────────────────────
	 * The rig refuses a mask toggle by construction and names the reason: a toggle re-reads mask
	 * state at press time from a document up to a second old, so the thumb that meant "hide me" can
	 * un-hide instead. The firmware fixed its half by making actMaskOn/actMaskOff two functions
	 * whose direction is fixed when the button is DRAWN, and these two are that. The console's K key
	 * is still a toggle and is deliberately not ported. */
	QWidget *maskRow = new QWidget;
	QHBoxLayout *mask = new QHBoxLayout(maskRow);
	mask->setContentsMargins(0, 0, 0, 0);
	mask->setSpacing(ROW_GAP);
	m_maskVision = MakeButton(QStringLiteral("VISION"), "qciRigMaskVision");
	m_maskVision->setCheckable(true);
	m_maskVision->setMinimumHeight(MASK_H);
	connect(m_maskVision, &QPushButton::clicked, this,
		[]() { QCiRigClient::Get()->actMask(QStringLiteral("vision")); });
	m_maskBlock = MakeButton(QStringLiteral("BLOCK"), "qciRigMaskBlock");
	m_maskBlock->setCheckable(true);
	m_maskBlock->setMinimumHeight(MASK_H);
	connect(m_maskBlock, &QPushButton::clicked, this,
		[]() { QCiRigClient::Get()->actMask(QStringLiteral("block")); });
	mask->addWidget(m_maskVision, 1);
	mask->addWidget(m_maskBlock, 1);
	col->addWidget(maskRow);

	/* ── BRB and MIRROR ─────────────────────────────────────────────────────────────────────────
	 * Both fixed-direction, for the reason every control in this block is: the panel renders from a
	 * document up to a second old, and the CLI and the deck drive both of these too. */
	QWidget *toggleRow = new QWidget;
	QHBoxLayout *toggles = new QHBoxLayout(toggleRow);
	toggles->setContentsMargins(0, 0, 0, 0);
	toggles->setSpacing(ROW_GAP);

	m_brb = MakeButton(QStringLiteral("BRB"), "qciRigBrb");
	m_brb->setCheckable(true);
	m_brb->setMinimumHeight(TOGGLE_H);
	m_brb->setToolTip(QStringLiteral("Cover the camera with the BRB card and mute the mic WITHOUT leaving "
					 "the scene. The share panel and the program scene stay live."));
	connect(m_brb, &QPushButton::clicked, this, [this]() {
		/* THE DIRECTION IS THE ONE THAT WAS DRAWN. `qciWant` is written by the renderer whenever
		   the state it inverts changes, so a press acts on what the button said rather than on what
		   the model happens to hold at the instant of the click. */
		QCiRigClient::Get()->actBrb(m_brb->property("qciWant").toBool());
	});
	toggles->addWidget(m_brb, 1);

	m_mirror = MakeButton(QStringLiteral("MIRROR"), "qciRigMirror");
	m_mirror->setCheckable(true);
	m_mirror->setMinimumHeight(TOGGLE_H);
	connect(m_mirror, &QPushButton::clicked, this, [this]() {
		QCiRigClient::Get()->actMirror(m_mirror->property("qciWant").toBool());
	});
	toggles->addWidget(m_mirror, 1);
	col->addWidget(toggleRow);

	/* ⚠️ MIRROR IS THREE-STATE AND THE THIRD STATE NEEDS A SENTENCE, NOT A GREY BUTTON. `mirror` is
	   null when the filter is not provisioned on every camera, when the angles disagree, or when the
	   link is down — three different reasons for one disabled control, and an operator fighting a
	   switch mid-stream needs to know which. The word goes here, under the button. */
	m_mirrorWhy = MakeLabel(QString(), "qciRigNote");
	m_mirrorWhy->setAlignment(Qt::AlignCenter);
	m_mirrorWhy->setWordWrap(true);
	ApplyLabelFont(m_mirrorWhy, 116.0);
	m_mirrorWhy->hide();
	col->addWidget(m_mirrorWhy);

	return block;
}

/* ══ THE PAGER ═════════════════════════════════════════════════════════════════════════════════ */

QWidget *QCiOperatorPane::buildPager()
{
	m_stack = new QStackedWidget;
	m_stack->setObjectName(QStringLiteral("qciOpStack"));

	QWidget *pager = new QWidget;
	pager->setObjectName(QStringLiteral("qciOpPages"));
	pager->setAttribute(Qt::WA_StyledBackground, true);
	pager->setFixedHeight(PAGER_H);
	QHBoxLayout *row = new QHBoxLayout(pager);
	row->setContentsMargins(0, 0, 0, 0);
	row->setSpacing(0);

	/* ⚠️ FIVE FLAT CHECKABLE BUTTONS IN AN EXCLUSIVE GROUP — EXPLICITLY NOT A QTabBar. A QTabBar is
	   the OBS look, and removing it is a large part of why this dock exists. */
	m_pageButtons = new QButtonGroup(this);
	m_pageButtons->setExclusive(true);

	struct PageSpec {
		const char *verb;
		const char *label;
		QCiRigPane *(*make)();
	};
	static const PageSpec PAGES[] = {
		{"queue", "QUEUE", []() -> QCiRigPane * { return new QCiRigQueuePane; }},
		{"react", "REACT", []() -> QCiRigPane * { return new QCiRigReactionsPane; }},
		{"chat", "CHAT", []() -> QCiRigPane * { return new QCiRigChatPane; }},
		{"tank", "TANK", []() -> QCiRigPane * { return new QCiRigTankPane; }},
		{"audio", "AUDIO", []() -> QCiRigPane * { return new QCiRigAudioPane; }},
	};

	for (const PageSpec &spec : PAGES) {
		QCiRigPane *page = spec.make();
		/* THE PAGE'S OWN NAME GOES; ITS FRESHNESS STAYS. The button below already says which page
		   this is. But the pane's status readout is not a duplicate of anything: QCiRigModel.hpp is
		   emphatic that the blocks are NOT one instant, and the dock title's freshness word is the
		   HUB's — it says nothing about whether THIS page's own reply has gone stale. */
		page->setNameless();
		const int index = m_stack->addWidget(page);
		m_pageIndex.insert(QString::fromUtf8(spec.verb), index);

		QPushButton *b = MakeButton(QString::fromUtf8(spec.label), "qciOpPageButton");
		b->setCheckable(true);
		b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		ApplyLabelFont(b, 110.0, true);
		m_pageButtons->addButton(b, index);
		row->addWidget(b, 1);
		connect(b, &QPushButton::clicked, this, [this, index]() { m_stack->setCurrentIndex(index); });
	}

	/* ⚠️ PAGE GATING IS WHAT MAKES FIVE PANES CHEAPER THAN FIVE DOCKS, AND IT IS FREE.
	   QCiRigClient::surfaceWanted() asks each registered pane whether it isVisible(), and a
	   QStackedWidget's non-current page answers false. So exactly one of /reactions, /chat and
	   /audio-routing is polled at a time instead of three, with no gating code here at all. /hub is
	   exempt and always polled — the privacy state has to be current whatever page is showing — and
	   /queue is wanted by this pane itself, because BRB and the goal both ride on it. */
	m_stack->setCurrentIndex(0);
	if (QAbstractButton *first = m_pageButtons->button(0)) {
		first->setChecked(true);
	}

	return pager;
}

/* ══ THE PINNED TRANSPORT FOOT ═════════════════════════════════════════════════════════════════ */

QWidget *QCiOperatorPane::buildTransport()
{
	QWidget *foot = new QWidget;
	foot->setObjectName(QStringLiteral("qciOpTransport"));
	foot->setAttribute(Qt::WA_StyledBackground, true);
	QVBoxLayout *col = new QVBoxLayout(foot);
	col->setContentsMargins(ROW_GAP, ROW_GAP, ROW_GAP, ROW_GAP);
	col->setSpacing(ROW_GAP);

	QWidget *streamRow = new QWidget;
	QHBoxLayout *stream = new QHBoxLayout(streamRow);
	stream->setContentsMargins(0, 0, 0, 0);
	stream->setSpacing(ROW_GAP);

	m_stream = MakeButton(QStringLiteral("GO LIVE"), "qciRigStream");
	m_stream->setMinimumHeight(STREAM_H);
	ApplyLabelFont(m_stream, 132.0, true);
	connect(m_stream, &QPushButton::clicked, this, [this]() {
		/* ⚠️ THE DIRECTION IS THE ONE THAT WAS DRAWN, read off the button rather than recomputed
		   from the model at press time. A stale press then does nothing instead of doing the
		   opposite — "it ends a broadcast that just started, or starts one the operator just ended
		   and walked away from." The rig refuses a stream toggle verb for the same reason. */
		QCiRigClient::Get()->actStream(!m_stream->property("qciLive").toBool());
	});
	m_go = MakeButton(QStringLiteral("GO"), "qciRigGo");
	m_go->setMinimumHeight(STREAM_H);
	m_go->setToolTip(QStringLiteral("Hand off from the boot hold. Meaningful only while the rig is holding "
					"at boot; anywhere else the rig answers 'not holding at boot', which is a "
					"no-op rather than a surprise cut to camera."));
	connect(m_go, &QPushButton::clicked, this, []() { QCiRigClient::Get()->actGo(); });
	stream->addWidget(m_stream, 2);
	stream->addWidget(m_go, 1);
	col->addWidget(streamRow);

	/* ── the scripted start ─────────────────────────────────────────────────────────────────────
	 * The sequencer lives in the SERVER, not in a script somebody launches — "a countdown that dies
	 * with its terminal is worse than none" — and it HOLDS at QCi Boot by design, because a
	 * countdown running out while the operator is away must not put an empty seat on stream. These
	 * three controls drive it; the phase readout below reports the REAL phase, which the StreamDock
	 * also drives. */
	QWidget *armRow = new QWidget;
	QHBoxLayout *arm = new QHBoxLayout(armRow);
	arm->setContentsMargins(0, 0, 0, 0);
	arm->setSpacing(TIGHT_GAP);

	m_armMins = new QSpinBox;
	m_armMins->setObjectName(QStringLiteral("qciRigArmMins"));
	/* ⚠️ THE RANGE STARTS AT ZERO AND ZERO MEANS START NOW. events/server.mjs parses
	   `q.min === undefined ? NaN : Number(q.min)` precisely because `Number(x) || DEFAULT` swallowed
	   a zero and turned "start now" into a fifteen-minute countdown. A minimum of 1 here would be
	   the same bug wearing a spin box. */
	m_armMins->setRange(0, 180);
	m_armMins->setValue(15);
	m_armMins->setSuffix(QStringLiteral(" MIN"));
	m_armMins->setFixedHeight(ARM_H);
	m_armMins->setButtonSymbols(QAbstractSpinBox::NoButtons);
	m_armMins->setToolTip(QStringLiteral("Minutes until QCi Boot. ZERO means start now — it is a value, "
					     "not a missing one."));
	ApplyReadoutFont(m_armMins, 13);

	m_arm = MakeButton(QStringLiteral("ARM"), "qciRigArm");
	m_arm->setFixedHeight(ARM_H);
	connect(m_arm, &QPushButton::clicked, this,
		[this]() { QCiRigClient::Get()->actArm(m_armMins->value()); });

	m_cancel = MakeButton(QStringLiteral("CANCEL"), "qciRigCancel");
	m_cancel->setFixedHeight(ARM_H);
	connect(m_cancel, &QPushButton::clicked, this, []() { QCiRigClient::Get()->actArmCancel(); });

	m_phase = MakeLabel(QString(), "qciRigPhase");
	m_phase->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	ApplyReadoutFont(m_phase, 13);

	arm->addWidget(m_armMins);
	arm->addWidget(m_arm);
	arm->addWidget(m_cancel);
	arm->addWidget(m_phase, 1);
	col->addWidget(armRow);

	m_say = MakeLabel(QStringLiteral("READY"), "qciRigSay");
	m_say->setWordWrap(true);
	ApplyLabelFont(m_say, 114.0);
	col->addWidget(m_say);

	return foot;
}

/* ══ PAGES ═════════════════════════════════════════════════════════════════════════════════════ */

void QCiOperatorPane::showPage(const QString &name)
{
	const auto it = m_pageIndex.constFind(name);
	if (it == m_pageIndex.constEnd()) {
		return;
	}
	m_stack->setCurrentIndex(*it);
	if (QAbstractButton *b = m_pageButtons->button(*it)) {
		QSignalBlocker block(b);
		b->setChecked(true);
	}
}

/* ══ RENDER ════════════════════════════════════════════════════════════════════════════════════ */

void QCiOperatorPane::onModelChanged()
{
	renderModel(QCiRigClient::Get()->model());
}

void QCiOperatorPane::renderModel(const Model &model)
{
	QCiRigClient *client = QCiRigClient::Get();
	const Hub &h = model.hub;
	const Queue &q = model.queue;
	const Obs &obs = model.obs;
	const bool fresh = h.fresh.ever && h.fresh.fault.isEmpty();

	/* ── the freshness word, for the dock's own title bar ────────────────────────────────────────
	 * THREE STATES, NEVER TWO. "Never answered", "current" and "answered a while ago" are different
	 * rig states, and collapsing the first and third is how a panel shows an empty queue that is
	 * actually an unreachable server. */
	QString word;
	QString tone;
	if (!h.fresh.ever) {
		word = QStringLiteral("NO LINK");
		tone = QStringLiteral("bad");
	} else if (!fresh) {
		word = QStringLiteral("STALE %1").arg(FormatAge(client->nowMs() - h.fresh.at));
		tone = QStringLiteral("warn");
	} else {
		word = QStringLiteral("LIVE");
		tone = QStringLiteral("go");
	}
	if (word != m_freshness) {
		m_freshness = word;
		emit freshnessChanged(word, tone);
	}

	/* ── the privacy readout ────────────────────────────────────────────────────────────────────
	 * FOUR STATES, AND THE FOURTH IS THE ONE THAT FAILS SILENTLY. `mask` is absent when the QC
	 * Vision filter is not provisioned or the link dropped mid-read — drawing that as either mode
	 * "would put a PRIVACY-ON badge over a camera nobody has masked, which is the one lie this panel
	 * exists to not tell." But QCiRigClient::finish() deliberately KEEPS a block's last good content
	 * and only stamps `fresh.fault` on it, so once the rig stops answering, `mask` holds whatever it
	 * last said, FOREVER — and a naive render keeps drawing a green CAM BLOCKED off a fact nobody
	 * has confirmed since the events server died, the BaseUrl went wrong, or OBS came back without
	 * the filter. That is the same lie with a slower fuse, and the small STALE word in the title bar
	 * does not undo it: this is the bold centred label on the layer that cannot scroll away, and it
	 * is what the eye takes. So a stale block reads UNVERIFIED, in amber, and never as a mode. It
	 * may REPORT what was last seen; it may not ASSERT it. */
	{
		QSignalBlocker blockVision(m_maskVision);
		QSignalBlocker blockBlock(m_maskBlock);
		m_maskVision->setChecked(h.mask && *h.mask == QLatin1String("vision"));
		m_maskBlock->setChecked(h.mask && *h.mask == QLatin1String("block"));
	}
	/* ⚠️ THE MASK BUTTONS ARE NOT GATED ON `fresh`, on purpose, and for the same reason the panic
	   button is not either: they are the controls that TAKE the camera away, and a stale document is
	   exactly when the operator most needs to press one. The readout goes honest; the way out stays
	   pressable. */
	const bool maskKnown = h.mask.has_value();
	m_maskVision->setEnabled(maskKnown || !h.fresh.ever);
	m_maskBlock->setEnabled(maskKnown || !h.fresh.ever);

	if (!h.mask) {
		m_privacy->setText(QStringLiteral("MASK UNKNOWN — NOT PROVISIONED, OR OBS IS DOWN"));
		SetTone(m_privacy, QStringLiteral("bad"));
	} else if (!fresh) {
		QString last = QStringLiteral("CAM LIVE");
		if (h.held && *h.held) {
			last = QStringLiteral("HELD");
		} else if (*h.mask == QLatin1String("block")) {
			last = QStringLiteral("CAM BLOCKED");
		}
		m_privacy->setText(QStringLiteral("UNVERIFIED — LAST KNOWN %1, %2")
					   .arg(last, FormatAge(client->nowMs() - h.fresh.at)));
		SetTone(m_privacy, QStringLiteral("warn"));
	} else if (h.held && *h.held) {
		m_privacy->setText(QStringLiteral("HELD — PROGRAM IS %1").arg(h.hold ? *h.hold : QString()));
		SetTone(m_privacy, QStringLiteral("go"));
	} else if (*h.mask == QLatin1String("block")) {
		m_privacy->setText(QStringLiteral("CAM BLOCKED"));
		SetTone(m_privacy, QStringLiteral("go"));
	} else {
		m_privacy->setText(QStringLiteral("CAM LIVE — QC VISION"));
		SetTone(m_privacy, QString());
	}

	/* THE PANIC BUTTON IS NEVER DISABLED BY A MISSING FACT. Every other control here greys out when
	   the rig cannot say what it would do; this one does not, because a panic button whose
	   precondition is "the allowlist parsed" is a panic button that fails precisely when things are
	   already wrong. The rig's own hold verb is ungated for the same reason. */

	/* ── did the hold actually stick ────────────────────────────────────────────────────────────
	 * See the header. While the watch is armed, the hold scene is compared against what OBS reports
	 * as program. A mismatch is not a cosmetic disagreement: it means the panic's scene switch has
	 * been undone and the operator's face may be back on program while this pane still shows the
	 * button they pressed. */
	if (m_holdWatchUntil != 0) {
		if (client->nowMs() > m_holdWatchUntil) {
			m_holdWatchUntil = 0;
			m_holdScene.clear();
		} else if (fresh && h.scene && !m_holdScene.isEmpty() && *h.scene != m_holdScene) {
			m_holdAlarm->setText(QStringLiteral("⚠ HOLD DID NOT STICK — PROGRAM IS %1, NOT %2. "
							    "THE MASK AND THE VCAM CUT HELD; THE SCENE DID NOT.")
						     .arg(*h.scene, m_holdScene));
			SetTone(m_holdAlarm, QStringLiteral("bad"));
			m_holdAlarm->show();
			/* LATCHED. The alarm stays up until the next hold clears it: an operator who looked
			   away for two seconds must still find out. */
			m_holdWatchUntil = 0;
			m_holdScene.clear();
		}
	}
	m_vcam->setVisible(!m_vcam->text().isEmpty());

	/* ── BRB ────────────────────────────────────────────────────────────────────────────────────
	 * State from /queue.brbcam, which is where the CLI writes it back through /brbcam-sync so the
	 * panel tracks `rig brbcam` as well as its own presses. Absent means /queue has not answered, or
	 * answered something that was not a boolean — drawn as unknown, never as "not on air". */
	if (!q.brb) {
		/* DISABLED, NOT DRAWN AS OFF. The direction a fixed-direction button sends is decided from
		   the state it is drawn against, so a button drawn against no state has no direction to
		   send — leaving it pressable would make it a coin toss between covering the camera and
		   uncovering it. Same rule as MIRROR below, arrived at from the same place. */
		m_brb->setEnabled(false);
		{
			QSignalBlocker block(m_brb);
			m_brb->setChecked(false);
		}
		m_brb->setProperty("qciWant", true);
		m_brb->setText(QStringLiteral("BRB %1").arg(QString::fromUtf8(QCiUi::ABSENT)));
		SetTone(m_brb, QStringLiteral("bad"));
	} else {
		m_brb->setEnabled(true);
		{
			QSignalBlocker block(m_brb);
			m_brb->setChecked(*q.brb);
		}
		/* THE DIRECTION IS FIXED HERE, WHERE THE BUTTON IS DRAWN, and the label says it. */
		m_brb->setProperty("qciWant", !*q.brb);
		m_brb->setText(*q.brb ? QStringLiteral("BRB ON — RETURN") : QStringLiteral("BRB"));
		SetTone(m_brb, *q.brb ? QStringLiteral("go") : QString());
	}

	/* ── MIRROR: THREE STATES, AND null IS NOT false ─────────────────────────────────────────────
	 * events/server.mjs is explicit: "null means 'no answer': the filter is not provisioned on every
	 * camera, the angles disagree, or the link is down. The panel must render that as a DISABLED
	 * control, NOT as off — an off-looking switch that cannot be turned on is the kind of thing an
	 * operator fights with mid-stream." So the disabled state carries a reason word rather than
	 * being a grey rectangle. */
	if (!obs.mirror) {
		m_mirror->setEnabled(false);
		{
			QSignalBlocker block(m_mirror);
			m_mirror->setChecked(false);
		}
		m_mirror->setProperty("qciWant", true);
		m_mirror->setText(QStringLiteral("MIRROR"));
		SetTone(m_mirror, QStringLiteral("bad"));
		/* THE REASON, NAMED. `ok:false` on /obs means OBS is unreachable and the route says so in
		   its own words; `ok:true` with a null mirror means OBS is up and the FILTER is the problem.
		   Those want completely different things from the operator and only one is fixed by looking
		   at OBS. */
		if (!obs.fresh.ever) {
			m_mirrorWhy->setText(QStringLiteral("NO ANSWER — WAITING FOR RIG"));
		} else if (!obs.ok) {
			m_mirrorWhy->setText(obs.error.isEmpty() ? QStringLiteral("NO ANSWER — OBS DOWN")
								 : QStringLiteral("NO ANSWER — %1").arg(obs.error.toUpper()));
		} else {
			m_mirrorWhy->setText(QStringLiteral("NO ANSWER — NOT PROVISIONED ON EVERY CAMERA, "
							    "OR THE ANGLES DISAGREE"));
		}
		m_mirrorWhy->show();
	} else {
		m_mirror->setEnabled(true);
		{
			QSignalBlocker block(m_mirror);
			m_mirror->setChecked(*obs.mirror);
		}
		m_mirror->setProperty("qciWant", !*obs.mirror);
		m_mirror->setText(*obs.mirror ? QStringLiteral("MIRROR ON") : QStringLiteral("MIRROR"));
		SetTone(m_mirror, *obs.mirror ? QStringLiteral("go") : QString());
		m_mirrorWhy->hide();
	}

	/* ── transport ──────────────────────────────────────────────────────────────────────────────
	 * ON AIR CHANGES THE BUTTON'S IDENTITY, NOT JUST ITS COLOUR: the uptime goes IN the button, so
	 * the same object that would end the broadcast is also the object that says how long it has been
	 * running. The clock is the SERVER's duration (/hub sinceMs) and never one this process
	 * accumulated — an app restarted mid-stream would otherwise show a confidently wrong clock, and
	 * a confidently wrong clock is worse than no clock on a rig whose rule is "never show green over
	 * a dead link". */
	if (!h.live) {
		m_stream->setEnabled(false);
		m_stream->setText(QStringLiteral("STREAM %1").arg(QString::fromUtf8(QCiUi::ABSENT)));
		m_stream->setProperty("qciLive", false);
		SetTone(m_stream, QStringLiteral("bad"));
	} else if (*h.live) {
		m_stream->setEnabled(fresh);
		m_stream->setProperty("qciLive", true);
		m_stream->setText(h.sinceMs ? QStringLiteral("ON AIR  %1").arg(FormatDuration(*h.sinceMs))
					    : QStringLiteral("ON AIR"));
		SetTone(m_stream, QStringLiteral("live"));
	} else {
		m_stream->setEnabled(fresh);
		m_stream->setProperty("qciLive", false);
		m_stream->setText(QStringLiteral("GO LIVE"));
		SetTone(m_stream, QStringLiteral("go"));
	}
	/* ⚠️ A CHANGED DYNAMIC PROPERTY DOES NOT RESTYLE ON ITS OWN. `qciLive` is read by the click
	   handler rather than by the theme; `qciTone` above is read by the theme and SetTone() does the
	   unpolish/polish. Do not "simplify" one into the other. */

	/* GO IS MEANINGFUL ONLY WHILE THE RIG IS HOLDING AT BOOT, and the word for that phase is the
	   rig's, not ours: goLive() refuses with "not holding at boot" unless
	   `state.startSeq.phase === "boot"`. The button is lit only in that phase rather than being a
	   control that usually answers no. */
	const bool holdingAtBoot = h.phase && *h.phase == QLatin1String("boot");
	m_go->setEnabled(holdingAtBoot);

	/* ARM is offered whenever the rig is answering; CANCEL only while there is a sequence to cancel.
	   A CANCEL that is always lit is a CANCEL whose refusal the operator learns to expect. */
	m_arm->setEnabled(fresh);
	m_armMins->setEnabled(fresh);
	m_cancel->setEnabled(fresh && h.phase.has_value());

	if (h.phase) {
		/* THE PHASE IS THE RIG'S, NOT WHAT THIS PANEL LAST DID — the StreamDock drives the same
		   sequence. "soon" counts down; "boot" is the hold that nothing leaves without /go. */
		m_phase->setText(h.inMs ? QStringLiteral("%1 · %2")
						  .arg(h.phase->toUpper(), FormatDuration(*h.inMs))
					: h.phase->toUpper());
		SetTone(m_phase, holdingAtBoot ? QStringLiteral("warn") : QString());
	} else {
		m_phase->setText(QStringLiteral("IDLE"));
		SetTone(m_phase, QString());
	}
}
