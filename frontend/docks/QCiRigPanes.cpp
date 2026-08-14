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

#include "QCiRigPanes.hpp"

#include "QCiRigUi.hpp"

#include <widgets/QCiBasic.hpp>

#include <QDoubleSpinBox>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyle>
#include <QVBoxLayout>

#ifdef BROWSER_AVAILABLE
/* BrowserDock.hpp is the fork's one declaration of `cef` and `panel_cookies`; declaring them again
   here would be a second spelling of a global whose lifetime rules are subtle. */
#include <docks/BrowserDock.hpp>
#endif

#include "moc_QCiRigPanes.cpp"

using namespace QCiRig;

/* ⚠️ THE HOUSE VOCABULARY MOVED OUT OF THIS FILE. SetTone, ApplyLabelFont, MakeLabel, MakeButton,
 * MakeCard, ClearLayout and ABSENT are now in QCiRigUi.hpp.
 *
 * They used to be an anonymous namespace here, which meant exactly one translation unit could reach
 * them. The flight strip, the command bar, the scene rail, the audio strip and the operator section
 * are five MORE surfaces that have to look like the same application, and the way five surfaces
 * stop looking like one application is each one growing its own private copy of "how a label is
 * drawn here". Promoted, not copied — nothing below re-implements one. */
using QCiUi::ABSENT;
using QCiUi::ApplyLabelFont;
using QCiUi::ClearLayout;
using QCiUi::MakeButton;
using QCiUi::MakeCard;
using QCiUi::MakeLabel;
using QCiUi::SetTone;

namespace {

/* THE GOAL METER'S GEOMETRY. 40 cells is the console's own count and the number is the language:
   the rig's meters are stepped everywhere — the ESP32 panels, the tank's 32x16 matrix, the audio
   strip's 18 cells — and a smooth bar in the middle of that reads as a borrowed control. */
constexpr int GOAL_METER_CELLS = 40;
constexpr int GOAL_METER_H = 14;

/** The placeholder a list shows when it is genuinely empty. Distinct wording per list, because
 *  "NOTHING WAITING" and "the rig has not answered" are different states and the second one is
 *  reported by the pane's status readout, never by this. Stays local: it is about a LIST, and the
 *  three lists that use it are all in this file. */
QLabel *EmptyRow(const QString &text)
{
	QLabel *l = MakeLabel(text, "qciRigEmpty");
	l->setAlignment(Qt::AlignCenter);
	ApplyLabelFont(l, 118.0);
	return l;
}

QString OrAbsent(const Maybe<double> &v, int decimals, const QString &suffix = QString())
{
	if (!v) {
		return QString::fromUtf8(ABSENT);
	}
	return QString::number(*v, 'f', decimals) + suffix;
}

} // namespace

/* ══ the shared pane ═══════════════════════════════════════════════════════════════════════════ */

QCiRigPane::QCiRigPane(QCiRigClient::Surface surface, const QString &title, QWidget *parent) : QWidget(parent)
{
	/* ⚠️ WITHOUT THIS THE THEME DOES NOT REACH THIS WIDGET AT ALL, and the failure is silent.
	   Qt only honours a stylesheet background on a QWidget SUBCLASS if the subclass either
	   reimplements paintEvent() or carries WA_StyledBackground — otherwise QCi.obt's rule for
	   `QCiRigPane` parses, resolves, matches, and paints nothing, and the panel comes up in the
	   default window grey inside an otherwise themed application. QFrame (every card and row below)
	   does not need it; a bare QWidget does. */
	setAttribute(Qt::WA_StyledBackground, true);

	QVBoxLayout *outer = new QVBoxLayout(this);
	outer->setContentsMargins(8, 6, 8, 8);
	outer->setSpacing(6);
	m_outer = outer;

	/* ── the title row ──────────────────────────────────────────────────────────────────────── */
	QWidget *header = new QWidget;
	header->setObjectName(QStringLiteral("qciRigPaneHeader"));
	QHBoxLayout *headerRow = new QHBoxLayout(header);
	headerRow->setContentsMargins(0, 0, 0, 0);
	headerRow->setSpacing(8);

	m_title = MakeLabel(title, "qciRigPaneTitle");
	ApplyLabelFont(m_title, 122.0, true);
	headerRow->addWidget(m_title);

	m_rule = new QFrame;
	m_rule->setObjectName(QStringLiteral("qciRigPaneRule"));
	m_rule->setFrameShape(QFrame::HLine);
	m_rule->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	headerRow->addWidget(m_rule, 1);

	m_status = MakeLabel(QString(), "qciRigPaneStatus");
	m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	ApplyLabelFont(m_status, 116.0);
	headerRow->addWidget(m_status);

	m_header = header;
	outer->addWidget(header);

	/* ── the layer that cannot scroll away ──────────────────────────────────────────────────── */
	m_pinned = new QVBoxLayout;
	m_pinned->setContentsMargins(0, 0, 0, 0);
	m_pinned->setSpacing(6);
	outer->addLayout(m_pinned);

	/* ── everything else ────────────────────────────────────────────────────────────────────── */
	QScrollArea *scroll = new QScrollArea;
	scroll->setObjectName(QStringLiteral("qciRigPaneScroll"));
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	QWidget *bodyWidget = new QWidget;
	bodyWidget->setObjectName(QStringLiteral("qciRigPaneBody"));
	m_body = new QVBoxLayout(bodyWidget);
	m_body->setContentsMargins(0, 0, 0, 0);
	m_body->setSpacing(6);
	scroll->setWidget(bodyWidget);
	outer->addWidget(scroll, 1);
	m_scroll = scroll;

	/* THE ONLY SUBSCRIPTION A PANE MAKES. It declares which surface it draws so the client can skip
	   polling a surface nobody is looking at, and it renders when the model moves. It does not ask
	   for anything, and it has no timer. */
	QCiRigClient *client = QCiRigClient::Get();
	client->wantSurface(surface, this);
	connect(client, &QCiRigClient::changed, this, &QCiRigPane::onChanged);
}

void QCiRigPane::onChanged()
{
	renderModel(rig()->model());
}

void QCiRigPane::setNameless()
{
	/* The NAME goes; the FRESHNESS stays. See the header for why those are two decisions: the pager
	   above renames the page for free, but nothing above this pane knows whether THIS block's reply
	   is current, and the model's own header is emphatic that the blocks are not one instant. The
	   status label is stretched into the space the title had so the age has somewhere to be read. */
	m_title->hide();
	m_rule->hide();
	m_status->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
}

void QCiRigPane::pinBodyFullBleed()
{
	/* The scroll area is HIDDEN rather than deleted so that body() stays a valid layout — a pane
	   that opts out of scrolling should not turn every later body()->addWidget() into a crash. */
	m_scroll->hide();
	m_outer->setStretchFactor(m_scroll, 0);
	m_outer->setStretchFactor(m_pinned, 1);
}

void QCiRigPane::setStatus(const QString &text, const QString &tone)
{
	if (m_status->text() != text) {
		m_status->setText(text);
	}
	SetTone(m_status, tone);
}

bool QCiRigPane::applyFreshness(const Freshness &fresh)
{
	if (!fresh.ever) {
		/* NEVER ANSWERED. Not "empty" — the difference is the whole point: an empty moderation
		   queue and a server that has never spoken look identical in a list and mean opposite
		   things about whether there is work waiting. */
		setStatus(fresh.fault.isEmpty() ? QStringLiteral("WAITING FOR RIG") : QStringLiteral("RIG UNREACHABLE"),
			  QStringLiteral("bad"));
		return false;
	}
	if (!fresh.fault.isEmpty()) {
		/* Answered before, failing now. The pane keeps its last good content and says how old it
		   is — blanking would read as "the queue cleared". */
		setStatus(QStringLiteral("STALE %1").arg(FormatAge(rig()->nowMs() - fresh.at)), QStringLiteral("warn"));
		return false;
	}
	return true;
}

/* ══ THE GOAL METER ════════════════════════════════════════════════════════════════════════════ */

QCiGoalMeter::QCiGoalMeter(QWidget *parent) : QWidget(parent)
{
	setFixedHeight(GOAL_METER_H);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void QCiGoalMeter::setGoal(const Maybe<Goal> &goal)
{
	/* Repaint only on an actual change. This widget is asked to render on every /queue poll and a
	   meter that repaints once a second forever is a meter costing the compositor for nothing. */
	const bool same = (!goal && !m_goal) ||
			  (goal && m_goal && goal->current == m_goal->current && goal->target == m_goal->target);
	if (same) {
		return;
	}
	m_goal = goal;
	update();
}

void QCiGoalMeter::paintEvent(QPaintEvent *)
{
	QPainter p(this);

	const int cells = GOAL_METER_CELLS;
	const int gap = 2;
	const qreal cellW = (qreal(width()) - qreal(gap * (cells - 1))) / qreal(cells);
	if (cellW <= 0.0) {
		return;
	}

	/* ⚠️ ABSENT IS AN EMPTY RAIL, NOT ZERO CELLS OF SOMETHING. A meter reading "0 of $20" is a claim
	   that the run has started and nobody has given anything; a rig that has not published a goal has
	   made no such claim. Both draw as unlit cells here, but only one of them prints a figure beside
	   the meter (see the queue pane), and that is the difference the operator reads. */
	int lit = 0;
	int over = 0;
	if (m_goal && m_goal->target > 0.0) {
		const double ratio = m_goal->current / m_goal->target;
		lit = qBound(0, int(ratio * cells), cells);
		/* PAST THE TARGET IS ITS OWN STATE. Clamping a 140% run to "full" throws away the best
		   thing that can happen on this meter. */
		if (ratio > 1.0) {
			over = cells;
		}
	}

	for (int i = 0; i < cells; i++) {
		const QRectF cell(qreal(i) * (cellW + gap), 0.0, cellW, qreal(height()));
		const QColor &c = over ? m_over : (i < lit ? m_filled : m_unlit);
		p.fillRect(cell, c);
	}
}

/* ══ QUEUE ═════════════════════════════════════════════════════════════════════════════════════ */

QCiRigQueuePane::QCiRigQueuePane(QWidget *parent)
	: QCiRigPane(QCiRigClient::SurfaceQueue, QStringLiteral("MODERATION"), parent)
{
	QWidget *topRow = new QWidget;
	QHBoxLayout *top = new QHBoxLayout(topRow);
	top->setContentsMargins(0, 0, 0, 0);
	top->setSpacing(6);

	/* MASTER AUTO — the "just let them flow" switch, and it is amber because it is safe but
	   consequential. NO ARGUMENT is sent: the rig decides the direction, because "if the deck
	   flipped this a moment ago our label is stale and we would command the state it is already
	   in." Turning it OFF restores the per-platform rules; it is not an all-off. */
	m_auto = MakeButton(QStringLiteral("AUTO"), "qciRigAuto");
	m_auto->setCheckable(true);
	m_auto->setToolTip(QStringLiteral("Approve everything automatically. OFF restores your "
					  "per-platform rules — it is not an all-off."));
	connect(m_auto, &QPushButton::clicked, this, [this]() { rig()->actAutoToggle(); });

	m_approveAll = MakeButton(QStringLiteral("APPROVE ALL"), "qciRigApproveAll");
	connect(m_approveAll, &QPushButton::clicked, this, [this]() { rig()->actApproveAll(); });

	/* ⚠️ THE LABEL IS THE WARNING, AND IT IS ON THE BUTTON RATHER THAN IN A TOOLTIP. GET /event puts
	   a REAL $5 superchat on the queue and the goal counts it. The console calls this button "TEST",
	   which is exactly the word that makes an operator press it to see what happens. A tooltip is
	   not read before a press; a face is. */
	m_inject = MakeButton(QStringLiteral("INJECT REAL $5"), "qciRigInject");
	m_inject->setToolTip(QStringLiteral("Injects a REAL paid item into the moderation queue and adds "
					    "$5 to the goal. There is no test mode — this is the live path."));
	connect(m_inject, &QPushButton::clicked, this, [this]() { rig()->actInjectPaidTestItem(); });

	top->addWidget(m_auto, 1);
	top->addWidget(m_approveAll, 2);
	top->addWidget(m_inject, 2);
	body()->addWidget(topRow);

	/* ── GOAL ───────────────────────────────────────────────────────────────────────────────────
	 * MONEY. This is the one block in the application allowed to wear pink, and the theme spends it
	 * here and on the moderation card's amount and nowhere else. */
	QWidget *goalHeadRow = new QWidget;
	QHBoxLayout *goalHeadCells = new QHBoxLayout(goalHeadRow);
	goalHeadCells->setContentsMargins(0, 0, 0, 0);
	goalHeadCells->setSpacing(8);
	QLabel *goalHead = MakeLabel(QStringLiteral("GOAL"), "qciRigSubhead");
	ApplyLabelFont(goalHead, 120.0, true);
	goalHeadCells->addWidget(goalHead);
	/* THE FIGURE IS THE ONE PLACE BESIDES A DONATION CARD WHERE PINK IS LEGAL, and it is Menlo
	   because it is a number the operator reads across a room. */
	m_goalFigure = MakeLabel(QString::fromUtf8(ABSENT), "qciRigGoalFigure");
	m_goalFigure->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	QCiUi::ApplyReadoutFont(m_goalFigure, 13, true);
	goalHeadCells->addWidget(m_goalFigure, 1);
	body()->addWidget(goalHeadRow);

	m_goalMeter = new QCiGoalMeter;
	m_goalMeter->setObjectName(QStringLiteral("qciRigGoalMeter"));
	body()->addWidget(m_goalMeter);

	QWidget *goalRow = new QWidget;
	QHBoxLayout *goal = new QHBoxLayout(goalRow);
	goal->setContentsMargins(0, 0, 0, 0);
	goal->setSpacing(4);

	auto money = [this](const char *objectName, double minimum) {
		QDoubleSpinBox *box = new QDoubleSpinBox;
		box->setObjectName(QString::fromUtf8(objectName));
		box->setRange(minimum, 1000000.0);
		box->setDecimals(0);
		box->setPrefix(QStringLiteral("$"));
		box->setButtonSymbols(QAbstractSpinBox::NoButtons);
		box->setKeyboardTracking(false);
		/* ⚠️ THE DIRTY FLAG IS SET FROM THE KEYBOARD, NOT FROM valueChanged. The reconciler below
		   writes these boxes on every poll and valueChanged fires for that too, which would mark the
		   field dirty forever the moment the rig first answered — and a permanently dirty field is a
		   field the rig can never correct. QAbstractSpinBox::textChanged is emitted for a programmatic
		   setValue() as well, so the reconciler blocks signals around its writes and this connection
		   therefore only ever hears the operator. */
		connect(box, &QDoubleSpinBox::textChanged, this, [this]() { m_goalDirty = true; });
		return box;
	};
	m_goalCurrent = money("qciRigGoalCurrent", 0.0);
	m_goalTarget = money("qciRigGoalTarget", 1.0);

	m_goalLabel = new QLineEdit;
	m_goalLabel->setObjectName(QStringLiteral("qciRigGoalLabel"));
	m_goalLabel->setPlaceholderText(QStringLiteral("WHAT FOR"));
	/* 24 IS THE RIG'S NUMBER (`q.label.slice(0, 24)`), mirrored here only so the box stops taking
	   characters it knows will be dropped. The rig still slices — this is a courtesy, not a rule. */
	m_goalLabel->setMaxLength(24);
	connect(m_goalLabel, &QLineEdit::textEdited, this, [this]() { m_goalDirty = true; });

	goal->addWidget(m_goalCurrent, 1);
	goal->addWidget(m_goalTarget, 1);
	goal->addWidget(m_goalLabel, 2);
	body()->addWidget(goalRow);

	QWidget *goalActs = new QWidget;
	QHBoxLayout *acts = new QHBoxLayout(goalActs);
	acts->setContentsMargins(0, 0, 0, 0);
	acts->setSpacing(4);
	m_goalSet = MakeButton(QStringLiteral("SET"), "qciRigGoalSet");
	connect(m_goalSet, &QPushButton::clicked, this, [this]() {
		m_goalSentCurrent = m_goalCurrent->value();
		m_goalSentTarget = m_goalTarget->value();
		rig()->actGoal(m_goalSentCurrent, m_goalSentTarget, m_goalLabel->text().trimmed());
	});
	m_goalClear = MakeButton(QStringLiteral("CLEAR"), "qciRigGoalClear");
	m_goalClear->setToolTip(QStringLiteral("Zero the current figure. The target and the label stay."));
	connect(m_goalClear, &QPushButton::clicked, this, [this]() {
		/* CLEAR IS ALSO A PUBLISH, so it drops the dirty flag's claim on `current` the same way SET
		   does — otherwise a cleared goal would keep showing whatever was typed before it. */
		m_goalSentCurrent = 0.0;
		m_goalSentTarget = m_goalTarget->value();
		rig()->actGoalClear();
	});
	acts->addWidget(m_goalSet, 1);
	acts->addWidget(m_goalClear, 1);
	body()->addWidget(goalActs);

	m_pendingCount = MakeLabel(QStringLiteral("PENDING 0"), "qciRigSubhead");
	ApplyLabelFont(m_pendingCount, 120.0, true);
	body()->addWidget(m_pendingCount);

	m_pendingBox = new QWidget;
	m_pendingList = new QVBoxLayout(m_pendingBox);
	m_pendingList->setContentsMargins(0, 0, 0, 0);
	m_pendingList->setSpacing(4);
	body()->addWidget(m_pendingBox);

	m_approvedCount = MakeLabel(QStringLiteral("QUEUED 0"), "qciRigSubhead");
	ApplyLabelFont(m_approvedCount, 120.0, true);
	body()->addWidget(m_approvedCount);

	m_approvedBox = new QWidget;
	m_approvedList = new QVBoxLayout(m_approvedBox);
	m_approvedList->setContentsMargins(0, 0, 0, 0);
	m_approvedList->setSpacing(4);
	body()->addWidget(m_approvedBox);

	body()->addStretch(1);

	renderModel(rig()->model());
}

void QCiRigQueuePane::renderModel(const Model &model)
{
	const Queue &q = model.queue;
	const bool fresh = applyFreshness(q.fresh);

	{
		QSignalBlocker block(m_auto);
		m_auto->setChecked(q.autoAll);
	}
	SetTone(m_auto, q.autoAll ? QStringLiteral("warn") : QString());
	m_auto->setEnabled(fresh);
	m_approveAll->setEnabled(fresh && !q.pending.isEmpty());
	m_inject->setEnabled(fresh);

	/* ── the goal, reconciled ───────────────────────────────────────────────────────────────────
	 * ⚠️ THE DIRTY FLAG IS CLEARED ON PROOF, NOT ON A PRESS. What clears it is the RIG reporting the
	 * numbers that were sent — the same discipline the reactions pane uses for its draft box. A flag
	 * cleared on the press would hand the field straight back to the reconciler while the write was
	 * still in flight, and the very next poll (up to 800ms of old document) would put the OLD figure
	 * back under the operator's eyes as though the SET had been refused.
	 *
	 * The comparison is on the SERVER's clamped values, not on what was typed: the rig floors current
	 * at 0 and target at 1, so typing 0 into TARGET and waiting for "0" to come back would wait
	 * forever. qFuzzyCompare is wrong for a value that can legitimately be 0.0, hence the epsilon. */
	if (m_goalDirty && q.goal) {
		const bool landed = qAbs(q.goal->current - qMax(0.0, m_goalSentCurrent)) < 0.005 &&
				    qAbs(q.goal->target - qMax(1.0, m_goalSentTarget)) < 0.005;
		if (landed) {
			m_goalDirty = false;
		}
	}
	if (!m_goalDirty) {
		/* SIGNALS BLOCKED AROUND EVERY PROGRAMMATIC WRITE. QDoubleSpinBox::textChanged and
		   QLineEdit::setText both fire on a setValue()/setText() from here, and the dirty flag is
		   wired to the first of those — without the blockers the reconciler would mark its own write
		   as an operator edit and the field would be dirty forever after the rig first answered. */
		if (q.goal) {
			QSignalBlocker blockCurrent(m_goalCurrent);
			QSignalBlocker blockTarget(m_goalTarget);
			QSignalBlocker blockLabel(m_goalLabel);
			m_goalCurrent->setValue(q.goal->current);
			m_goalTarget->setValue(q.goal->target);
			if (m_goalLabel->text() != q.goal->label) {
				m_goalLabel->setText(q.goal->label);
			}
		}
	}
	SetTone(m_goalCurrent, m_goalDirty ? QStringLiteral("warn") : QString());
	SetTone(m_goalTarget, m_goalDirty ? QStringLiteral("warn") : QString());
	SetTone(m_goalLabel, m_goalDirty ? QStringLiteral("warn") : QString());

	m_goalMeter->setGoal(q.goal);
	if (q.goal) {
		m_goalFigure->setText(QStringLiteral("%1 / %2").arg(FormatMoney(q.goal->current),
								    FormatMoney(q.goal->target)));
		SetTone(m_goalFigure, QString());
	} else {
		/* NO GOAL PUBLISHED IS NOT A GOAL OF ZERO. "$0 / $20" asserts a run that has started and
		   raised nothing; an absent goal asserts nothing at all. */
		m_goalFigure->setText(QString::fromUtf8(ABSENT));
		SetTone(m_goalFigure, QStringLiteral("bad"));
	}
	m_goalSet->setEnabled(fresh);
	m_goalClear->setEnabled(fresh && q.goal.has_value());

	m_pendingCount->setText(QStringLiteral("PENDING %1").arg(q.pending.size()));
	m_approvedCount->setText(QStringLiteral("QUEUED %1").arg(q.approved.size()));

	/* ── pending ────────────────────────────────────────────────────────────────────────────── */
	/* ⚠️ THE COUNT IS IN THE SIGNATURE SO THAT AN EMPTY LIST HAS ONE. control.html hit this and
	   wrote it down: "seeded null for the same reason: an empty list's signature is '', so a ''
	   seed would make the first poll look unchanged and the EMPTY placeholder would never be
	   drawn." A default-constructed QString compares equal to an empty one, so without this the
	   very first render of an empty queue skips the rebuild and the pane shows nothing at all —
	   no rows and no "NOTHING WAITING", which reads as a pane that failed to load. */
	QString sig = QStringLiteral("n=%1;").arg(q.pending.size());
	for (const QueueItem &it : q.pending) {
		sig += QString::number(it.id);
		sig += QLatin1Char('\x1f');
	}
	if (sig != m_pendingSig) {
		m_pendingSig = sig;
		ClearLayout(m_pendingList);
		if (q.pending.isEmpty()) {
			m_pendingList->addWidget(EmptyRow(QStringLiteral("NOTHING WAITING")));
		} else {
			for (const QueueItem &it : q.pending) {
				QFrame *card = MakeCard("qciRigCard");
				QVBoxLayout *cardBox = new QVBoxLayout(card);
				cardBox->setContentsMargins(6, 5, 6, 5);
				cardBox->setSpacing(3);

				QWidget *whoRow = new QWidget;
				QHBoxLayout *who = new QHBoxLayout(whoRow);
				who->setContentsMargins(0, 0, 0, 0);
				who->setSpacing(6);
				QLabel *whoLabel = MakeLabel(it.who.isEmpty() ? QStringLiteral("?") : it.who,
							     "qciRigCardWho");
				ApplyLabelFont(whoLabel, 112.0, true);
				who->addWidget(whoLabel, 1);
				const QString amount = FormatAmount(it.amount, it.currency);
				if (!amount.isEmpty()) {
					QLabel *amountLabel = MakeLabel(amount, "qciRigCardAmount");
					who->addWidget(amountLabel);
				}
				cardBox->addWidget(whoRow);

				QStringList meta;
				if (!it.platform.isEmpty()) {
					meta << it.platform.toUpper();
				}
				if (!it.kind.isEmpty()) {
					meta << it.kind.toUpper();
				}
				if (it.tts) {
					/* TTS IS NAMED ON THE CARD BECAUSE APPROVING ONE SPEAKS IT ALOUD ON A
					   LIVE BROADCAST. It is the single most consequential difference
					   between two otherwise identical cards. */
					meta << QStringLiteral("TTS");
				}
				QLabel *metaLabel = MakeLabel(meta.join(QStringLiteral(" · ")), "qciRigCardMeta");
				ApplyLabelFont(metaLabel, 118.0);
				cardBox->addWidget(metaLabel);

				if (!it.text.isEmpty()) {
					QLabel *msg = MakeLabel(it.text, "qciRigCardText");
					msg->setWordWrap(true);
					/* PLAIN TEXT, ALWAYS. This string came from a viewer on the internet;
					   Qt's rich-text auto-detection would render markup out of it, and this
					   is a surface the operator reads aloud from. */
					msg->setTextFormat(Qt::PlainText);
					cardBox->addWidget(msg);
				}

				QWidget *actRow = new QWidget;
				QHBoxLayout *acts = new QHBoxLayout(actRow);
				acts->setContentsMargins(0, 0, 0, 0);
				acts->setSpacing(4);
				QPushButton *approve = MakeButton(QStringLiteral("APPROVE"), "qciRigApprove");
				const qint64 id = it.id;
				connect(approve, &QPushButton::clicked, this, [this, id]() { rig()->actApprove(id); });
				QPushButton *reject = MakeButton(QStringLiteral("REJECT"), "qciRigReject");
				connect(reject, &QPushButton::clicked, this, [this, id]() { rig()->actReject(id); });
				acts->addWidget(approve, 1);
				acts->addWidget(reject, 1);
				cardBox->addWidget(actRow);

				m_pendingList->addWidget(card);
			}
		}
	}

	/* ── approved ───────────────────────────────────────────────────────────────────────────── */
	QString approvedSig = QStringLiteral("n=%1;").arg(q.approved.size());
	for (const QueueItem &it : q.approved) {
		approvedSig += QString::number(it.id);
		approvedSig += QLatin1Char('\x1f');
	}
	if (approvedSig != m_approvedSig) {
		m_approvedSig = approvedSig;
		ClearLayout(m_approvedList);
		if (q.approved.isEmpty()) {
			m_approvedList->addWidget(EmptyRow(QStringLiteral("NOTHING QUEUED")));
		} else {
			for (const QueueItem &it : q.approved) {
				QFrame *row = MakeCard("qciRigRow");
				QHBoxLayout *cells = new QHBoxLayout(row);
				cells->setContentsMargins(6, 3, 6, 3);
				cells->setSpacing(6);
				QLabel *who = MakeLabel(it.who.isEmpty() ? QStringLiteral("?") : it.who, "qciRigCardWho");
				who->setTextFormat(Qt::PlainText);
				cells->addWidget(who, 1);
				const QString amount = FormatAmount(it.amount, it.currency);
				if (!amount.isEmpty()) {
					cells->addWidget(MakeLabel(amount, "qciRigCardAmount"));
				}
				QPushButton *pull = MakeButton(QStringLiteral("PULL"), "qciRigReject");
				const qint64 id = it.id;
				connect(pull, &QPushButton::clicked, this, [this, id]() { rig()->actReject(id); });
				cells->addWidget(pull);
				m_approvedList->addWidget(row);
			}
		}
	}

	if (fresh) {
		setStatus(q.pending.isEmpty() ? QStringLiteral("CLEAR")
					      : QStringLiteral("%1 WAITING").arg(q.pending.size()),
			  q.pending.isEmpty() ? QString() : QStringLiteral("warn"));
	}
}

/* ══ REACTIONS ═════════════════════════════════════════════════════════════════════════════════ */

QCiRigReactionsPane::QCiRigReactionsPane(QWidget *parent)
	: QCiRigPane(QCiRigClient::SurfaceReactions, QStringLiteral("REACTIONS"), parent)
{
	QWidget *addRow = new QWidget;
	QHBoxLayout *add = new QHBoxLayout(addRow);
	add->setContentsMargins(0, 0, 0, 0);
	add->setSpacing(6);
	m_url = new QLineEdit;
	m_url->setObjectName(QStringLiteral("qciRigReactionUrl"));
	m_url->setPlaceholderText(QStringLiteral("https://…"));
	m_url->setClearButtonEnabled(true);
	connect(m_url, &QLineEdit::returnPressed, this, &QCiRigReactionsPane::submitUrl);
	m_add = MakeButton(QStringLiteral("QUEUE"), "qciRigReactionAdd");
	connect(m_add, &QPushButton::clicked, this, &QCiRigReactionsPane::submitUrl);
	add->addWidget(m_url, 1);
	add->addWidget(m_add);
	body()->addWidget(addRow);

	/* ── the transport ──────────────────────────────────────────────────────────────────────────
	 * TWO KINDS OF VERB IN ONE ROW, AND THE SPLIT IS NOT COSMETIC. BACK/NEXT are QUEUE verbs: they
	 * move this rig's list and work on every platform, so they are always live. PLAY/PAUSE/SEEK/
	 * SPEED are TRANSPORT verbs, which exist only where the platform publishes a remote control —
	 * YouTube does, Twitch and Kick do not. On a platform that cannot be driven the whole group is
	 * HIDDEN and the reason is printed in its place, "because four buttons that quietly do nothing
	 * are found out mid-reaction, on air."
	 *
	 * ⚠️ WHETHER IT CAN BE DRIVEN IS THE SERVER'S ANSWER, never a guess about the URL here.
	 * lib/embed.mjs owns that capability table and a second copy in this file is exactly the drift
	 * qci-rig's lib/contracts.test.mjs exists to catch. */
	QWidget *queueRow = new QWidget;
	QHBoxLayout *queueVerbs = new QHBoxLayout(queueRow);
	queueVerbs->setContentsMargins(0, 0, 0, 0);
	queueVerbs->setSpacing(4);
	m_prev = MakeButton(QStringLiteral("◀◀ BACK"), "qciRigRxPrev");
	connect(m_prev, &QPushButton::clicked, this, [this]() { rig()->actReactionPrev(); });
	m_next = MakeButton(QStringLiteral("NEXT ▶▶"), "qciRigRxNext");
	connect(m_next, &QPushButton::clicked, this, [this]() { rig()->actReactionNext(); });
	queueVerbs->addWidget(m_prev, 1);
	queueVerbs->addWidget(m_next, 1);
	body()->addWidget(queueRow);

	m_transport = new QWidget;
	QHBoxLayout *transport = new QHBoxLayout(m_transport);
	transport->setContentsMargins(0, 0, 0, 0);
	transport->setSpacing(4);
	m_play = MakeButton(QStringLiteral("PLAY"), "qciRigRxPlay");
	connect(m_play, &QPushButton::clicked, this, [this]() { rig()->actReactionResume(); });
	m_pause = MakeButton(QStringLiteral("PAUSE"), "qciRigRxPause");
	connect(m_pause, &QPushButton::clicked, this, [this]() { rig()->actReactionPause(); });
	m_back10 = MakeButton(QStringLiteral("−10"), "qciRigRxBack");
	connect(m_back10, &QPushButton::clicked, this, [this]() { rig()->actReactionSeekBy(-10.0); });
	m_fwd10 = MakeButton(QStringLiteral("+10"), "qciRigRxFwd");
	connect(m_fwd10, &QPushButton::clicked, this, [this]() { rig()->actReactionSeekBy(10.0); });
	m_rate = MakeButton(QStringLiteral("1.00×"), "qciRigRxRate");
	connect(m_rate, &QPushButton::clicked, this, [this]() { rig()->actReactionRateStep(); });
	transport->addWidget(m_play, 1);
	transport->addWidget(m_pause, 1);
	transport->addWidget(m_back10, 1);
	transport->addWidget(m_fwd10, 1);
	transport->addWidget(m_rate, 1);
	body()->addWidget(m_transport);

	m_reason = MakeLabel(QString(), "qciRigNote");
	m_reason->setWordWrap(true);
	m_reason->hide();
	body()->addWidget(m_reason);

	m_listBox = new QWidget;
	m_list = new QVBoxLayout(m_listBox);
	m_list->setContentsMargins(0, 0, 0, 0);
	m_list->setSpacing(4);
	body()->addWidget(m_listBox);

	body()->addStretch(1);

	renderModel(rig()->model());
}

void QCiRigReactionsPane::submitUrl()
{
	const QString url = m_url->text().trimmed();
	if (url.isEmpty()) {
		return;
	}
	rig()->actReactionAdd(url);
	/* ⚠️ THE BOX IS NOT CLEARED HERE. A link the rig refuses is still the operator's draft, and
	   clearing it out from under them is worse than leaving it to be corrected — a refusal on this
	   surface is usually a link with no player embed, which /share would have accepted and then
	   rendered as an error string ON STREAM. It clears in renderModel(), and only once the list has
	   actually grown, which is the rig saying yes rather than this pane assuming it. */
	m_submitted = url;
	m_countAtSubmit = static_cast<int>(rig()->model().reactions.items.size());
}

void QCiRigReactionsPane::renderModel(const Model &model)
{
	const Reactions &r = model.reactions;
	const bool fresh = applyFreshness(r.fresh);

	m_prev->setEnabled(fresh);
	m_next->setEnabled(fresh);
	m_add->setEnabled(fresh);

	m_transport->setVisible(r.control);
	m_reason->setVisible(!r.control && r.currentId.has_value());
	m_reason->setText(r.reason);

	/* -10/+10 need a POSITION and the player does not always give one; they grey out rather than
	   seeking from a number nobody measured. */
	const bool canSeek = r.position.has_value();
	m_back10->setEnabled(canSeek);
	m_fwd10->setEnabled(canSeek);
	m_rate->setEnabled(!r.rates.isEmpty());
	m_rate->setText(QStringLiteral("%1×").arg(QString::number(r.rate, 'f', 2)));

	/* Signature carries the PLAYING id as well as the list: when a video ends the rig advances on
	   its own, with nobody touching this panel, and the ON AIR marker has to move with it. */
	QString sig = QStringLiteral("n=%1;").arg(r.items.size());
	for (const Reaction &it : r.items) {
		sig += it.id;
		sig += QLatin1Char(':');
		sig += it.status;
		sig += QLatin1Char('\x1f');
	}
	sig += QStringLiteral("cur=");
	sig += r.currentId ? *r.currentId : QString();

	/* THE DRAFT BOX, EMPTIED ON PROOF. Not on "the list changed" — the deck, the CLI, the wrapper
	   page and a video simply ENDING all move this list with nobody touching this panel, and any of
	   those would otherwise wipe a URL the operator was still typing. */
	if (!m_submitted.isEmpty() && r.fresh.ever && static_cast<int>(r.items.size()) > m_countAtSubmit) {
		if (m_url->text().trimmed() == m_submitted) {
			m_url->clear();
		}
		m_submitted.clear();
		m_countAtSubmit = -1;
	}

	if (sig != m_listSig) {
		m_listSig = sig;
		ClearLayout(m_list);
		if (r.items.isEmpty()) {
			m_list->addWidget(EmptyRow(QStringLiteral("NOTHING QUEUED")));
		} else {
			int idx = 0;
			const int last = static_cast<int>(r.items.size()) - 1;
			for (const Reaction &it : r.items) {
				QFrame *row = MakeCard("qciRigRow");
				QHBoxLayout *cells = new QHBoxLayout(row);
				cells->setContentsMargins(6, 3, 6, 3);
				cells->setSpacing(4);

				QLabel *title = MakeLabel(it.title.isEmpty() ? QStringLiteral("?") : it.title,
							  "qciRigCardWho");
				title->setTextFormat(Qt::PlainText);
				cells->addWidget(title, 1);

				const QString id = it.id;
				if (r.currentId && *r.currentId == it.id) {
					QLabel *onAir = MakeLabel(QStringLiteral("ON AIR"), "qciRigOnAir");
					ApplyLabelFont(onAir, 120.0, true);
					SetTone(onAir, QStringLiteral("live"));
					cells->addWidget(onAir);
				} else {
					QPushButton *play = MakeButton(QStringLiteral("▶"), "qciRigRowButton");
					connect(play, &QPushButton::clicked, this,
						[this, id]() { rig()->actReactionPlay(id); });
					cells->addWidget(play);
				}

				/* Reorder is INDEX-based because that is what the operator can see: the row
				   moves to where they pointed. The rig clamps, so UP on the top row is a
				   no-op rather than a refusal that loses the item. */
				QPushButton *up = MakeButton(QStringLiteral("▲"), "qciRigRowButton");
				const int upTo = qMax(0, idx - 1);
				connect(up, &QPushButton::clicked, this, [this, id, upTo]() {
					rig()->actReactionMove(id, upTo);
				});
				QPushButton *down = MakeButton(QStringLiteral("▼"), "qciRigRowButton");
				const int downTo = qMin(last, idx + 1);
				connect(down, &QPushButton::clicked, this, [this, id, downTo]() {
					rig()->actReactionMove(id, downTo);
				});
				QPushButton *drop = MakeButton(QStringLiteral("✕"), "qciRigRowButtonBad");
				connect(drop, &QPushButton::clicked, this,
					[this, id]() { rig()->actReactionRemove(id); });
				cells->addWidget(up);
				cells->addWidget(down);
				cells->addWidget(drop);

				m_list->addWidget(row);
				idx++;
			}
		}
	}

	if (fresh) {
		const QString state = r.currentId ? (r.state.isEmpty() ? QStringLiteral("ON REACTIONS")
								      : r.state.toUpper())
						  : (r.items.isEmpty() ? QStringLiteral("IDLE")
								       : QStringLiteral("QUEUED"));
		setStatus(r.upNext > 0 ? QStringLiteral("%1 +%2").arg(state).arg(r.upNext) : state);
	}
}

/* ══ CHAT ══════════════════════════════════════════════════════════════════════════════════════ */

QCiRigChatPane::QCiRigChatPane(QWidget *parent)
	: QCiRigPane(QCiRigClient::SurfaceChat, QStringLiteral("CHAT"), parent)
{
	m_placeholder = MakeLabel(QStringLiteral("CHAT — CONNECTING"), "qciRigEmpty");
	m_placeholder->setAlignment(Qt::AlignCenter);
	m_placeholder->setWordWrap(true);
	ApplyLabelFont(m_placeholder, 118.0);
	pinned()->addWidget(m_placeholder, 1);
	pinBodyFullBleed();

	renderModel(rig()->model());
}

void QCiRigChatPane::renderModel(const Model &model)
{
	const Chat &c = model.chat;
	/* ⚠️ THE FRESHNESS READOUT WINS OVER THE CONTENT READOUT. Both want the same one label, and the
	   pane's own status ("YOUTUBE") is the less important of the two: an embed URL that was true
	   ninety seconds ago is still an embed URL, but the operator needs to know the rig stopped
	   answering. Every other pane guards its setStatus() the same way. */
	const bool fresh = applyFreshness(c.fresh);

	if (!c.ok) {
		/* THE SERVER'S OWN REASON WORD, not a generic "unavailable" — "OBS is down" and "no chat
		   URL has ever been set" want completely different things from the operator, and only one
		   of them is fixed by looking at OBS. */
		const QString why = c.reason == QLatin1String("obs-down")
					    ? QStringLiteral("CHAT — OBS IS DOWN, SO THE EMBED URL CANNOT BE READ BACK")
				    : c.reason == QLatin1String("no-url")
					    ? QStringLiteral("CHAT — NO EMBED SET. RUN `rig chat <url>`")
					    : QStringLiteral("CHAT — WAITING FOR RIG");
		m_placeholder->setText(why);
		m_placeholder->show();
		if (m_view) {
			m_view->hide();
		}
		if (fresh) {
			setStatus(QString::fromUtf8(ABSENT), QStringLiteral("bad"));
		}
		return;
	}

	if (fresh) {
		setStatus(c.source.toUpper());
	}

#ifdef BROWSER_AVAILABLE
	/* ⚠️ VISIBILITY IS DECIDED BEFORE THE RELOAD DECISION, AND THE ORDER IS A BUG FIX.
	   Written the other way round, an OBS restart hid the view (the `!c.ok` branch above) and then
	   the recovery poll came back with the SAME url — so the early return below fired, the view
	   stayed hidden forever, and the pane showed a stale placeholder over a perfectly live embed.
	   Showing is idempotent; reloading is not. */
	if (m_view && !m_loadedUrl.isEmpty()) {
		m_view->show();
		m_placeholder->hide();
	}

	/* ⚠️ RELOAD ONLY ON AN ACTUAL CHANGE. This is somebody else's page with its own session and its
	   own scrollback; re-pointing it on every poll would restart it once a second and make chat
	   unreadable. The pane is polled — the embed is not. */
	if (m_loadedUrl == c.url) {
		return;
	}
	if (!cef) {
		m_placeholder->setText(QStringLiteral("CHAT — THIS BUILD HAS NO BROWSER PANEL. THE EMBED IS %1")
					       .arg(c.url));
		m_placeholder->show();
		return;
	}
	OBSBasic::InitBrowserPanelSafeBlock();
	if (!m_view) {
		QCefWidget *widget = cef->create_widget(this, c.url.toStdString(), panel_cookies);
		if (!widget) {
			m_placeholder->setText(QStringLiteral("CHAT — THE BROWSER PANEL WOULD NOT START"));
			m_placeholder->show();
			return;
		}
		m_view = widget;
		/* ⚠️ THE EMBED GOES ON THE UNSCROLLED LAYER. A QCefWidget is a NATIVE window
		   (BrowserDock sets WA_NativeWindow for the same reason), and a native subwindow inside a
		   QScrollArea viewport is a clipping hazard on macOS — it is composited by the window
		   server, not painted into the viewport, so it can draw over the scroll area's bounds. The
		   pane has nothing else to scroll anyway: the placeholder and the embed are its whole
		   body. */
		pinned()->addWidget(m_view, 1);
	} else {
		static_cast<QCefWidget *>(m_view)->setURL(c.url.toStdString());
	}
	m_loadedUrl = c.url;
	m_placeholder->hide();
	m_view->show();
#else
	/* No browser panel in this build. The URL is still printed, because it is the thing the
	   operator would paste somewhere else, and a pane that says only "unavailable" has withheld the
	   one fact it had. */
	if (m_loadedUrl != c.url) {
		m_loadedUrl = c.url;
		m_placeholder->setText(QStringLiteral("CHAT — %1\n%2").arg(c.source.toUpper(), c.url));
	}
	m_placeholder->show();
#endif
}

/* ══ TANK ══════════════════════════════════════════════════════════════════════════════════════ */

QCiRigTankPane::QCiRigTankPane(QWidget *parent)
	: QCiRigPane(QCiRigClient::SurfaceHub, QStringLiteral("QC · TANK"), parent)
{
	m_face = MakeLabel(QStringLiteral("TANK QUIET"), "qciRigFace");
	m_face->setAlignment(Qt::AlignCenter);
	ApplyLabelFont(m_face, 132.0, true);
	body()->addWidget(m_face);

	m_rosterBox = new QWidget;
	m_roster = new QGridLayout(m_rosterBox);
	m_roster->setContentsMargins(0, 0, 0, 0);
	m_roster->setSpacing(4);
	body()->addWidget(m_rosterBox);

	/* ── SHARE — one box for the whole application ──────────────────────────────────────────────
	 * The console has TWO of these (the CONTROL pane and the TANK tab) driving ONE server state, and
	 * the only reason they cannot disagree is that neither keeps state of its own. One box removes
	 * the question. It is on this page rather than in the pinned privacy block because putting a link
	 * on stream is not a privacy action, and the pinned layer is spent on the ones that are. */
	QLabel *shareHead = MakeLabel(QStringLiteral("SHARE ON REACTIONS"), "qciRigSubhead");
	ApplyLabelFont(shareHead, 120.0, true);
	body()->addWidget(shareHead);

	m_shareState = MakeLabel(QStringLiteral("IDLE"), "qciRigNote");
	m_shareState->setWordWrap(true);
	m_shareState->setTextFormat(Qt::PlainText);
	body()->addWidget(m_shareState);

	m_share = new QLineEdit;
	m_share->setObjectName(QStringLiteral("qciRigShareUrl"));
	m_share->setPlaceholderText(QStringLiteral("https://…"));
	m_share->setClearButtonEnabled(true);
	/* ⚠️ EDITED-SINCE-PUBLISHED, NOT FOCUSED. control.html measured the difference and it is a wrong
	   link ON STREAM: mousedown on LOAD moves focus to the button BEFORE the click handler runs —
	   60-150ms against an 800ms poll — so a focus-based guard lets the poll overwrite the typed URL,
	   and the handler then publishes the OLD link with both readouts saying ON REACTIONS and nothing
	   erroring anywhere. textEdited fires only for the keyboard; setText() from the reconciler does
	   not raise it. */
	connect(m_share, &QLineEdit::textEdited, this, [this]() { m_shareDirty = true; });
	body()->addWidget(m_share);

	QWidget *shareActs = new QWidget;
	QHBoxLayout *shareRow = new QHBoxLayout(shareActs);
	shareRow->setContentsMargins(0, 0, 0, 0);
	shareRow->setSpacing(4);
	m_shareLoad = MakeButton(QStringLiteral("LOAD"), "qciRigShareLoad");
	auto publish = [this]() {
		const QString url = m_share->text().trimmed();
		if (url.isEmpty()) {
			return;
		}
		/* WHAT WAS SENT, held so the dirty flag can be dropped on PROOF — the rig reporting this
		   exact URL back on /queue.share — rather than on the press. A refused link (the rig takes
		   http(s) only) therefore stays in the box as the operator's draft, which is the whole
		   reason submitUrl() in the reactions pane works the same way. */
		m_shareSent = url;
		rig()->actShare(url);
	};
	connect(m_shareLoad, &QPushButton::clicked, this, publish);
	connect(m_share, &QLineEdit::returnPressed, this, publish);
	m_shareClear = MakeButton(QStringLiteral("CLR"), "qciRigShareClear");
	m_shareClear->setToolTip(QStringLiteral("Put the idle card back on the reactions panel. Leaving the "
						 "last video frozen on screen reads as a stuck stream."));
	connect(m_shareClear, &QPushButton::clicked, this, [this]() {
		m_shareSent.clear();
		m_shareDirty = false;
		m_share->clear();
		rig()->actShareClear();
	});
	shareRow->addWidget(m_shareLoad, 1);
	shareRow->addWidget(m_shareClear, 1);
	body()->addWidget(shareActs);

	body()->addStretch(1);

	/* ⚠️ TWO SURFACES ON ONE PAGE, AND BOTH ARE DECLARED. The face and the roster come from /hub
	   (the base class registered that); the SHARE state rides on /queue, which is the panel's one
	   moderation poll. Declaring it here is what makes the client poll /queue while this page is on
	   screen — the pager gates the rest, so a page nobody is looking at costs nothing. */
	rig()->wantSurface(QCiRigClient::SurfaceQueue, this);

	renderModel(rig()->model());
}

void QCiRigTankPane::renderModel(const Model &model)
{
	const Hub &h = model.hub;
	applyFreshness(h.fresh);

	/* ⚠️ NO EMOTE MEANS THE TANK HAS GONE QUIET, AND THAT IS DRAWN AS SUCH. The rig drops the key
	   once the daemon's answer is older than its freshness window rather than publishing it stale,
	   because the failure it is preventing is already written down: "the overlay keeps showing the
	   last GIF it fetched, so the tank looks alive and simply stops changing." A calm mascot and a
	   dead link must not look the same. */
	if (h.emote) {
		m_face->setText(h.emote->toUpper());
		SetTone(m_face, QString());
	} else {
		m_face->setText(QStringLiteral("TANK QUIET"));
		SetTone(m_face, QStringLiteral("warn"));
	}

	/* ── SHARE, reconciled from /queue.share ────────────────────────────────────────────────────
	 * ⚠️ THIS RUNS BEFORE THE ROSTER SIGNATURE CHECK BELOW, WHICH RETURNS EARLY. Put after it, the
	 * share box would stop reconciling the moment the emote roster settled — which is within a second
	 * of launch and then forever. */
	const Queue &q = model.queue;
	if (m_shareDirty && !m_shareSent.isEmpty() && q.share && *q.share == m_shareSent) {
		/* PROOF: the rig is reporting the link that was sent. Only now does the reconciler get the
		   box back. */
		m_shareDirty = false;
		m_shareSent.clear();
	}
	if (!m_shareDirty) {
		const QString live = q.share ? *q.share : QString();
		if (m_share->text() != live) {
			QSignalBlocker block(m_share);
			m_share->setText(live);
		}
	}
	SetTone(m_share, m_shareDirty ? QStringLiteral("warn") : QString());

	if (!q.fresh.ever) {
		m_shareState->setText(QStringLiteral("SHARE — WAITING FOR RIG"));
		SetTone(m_shareState, QStringLiteral("bad"));
	} else if (q.share) {
		/* PLAIN TEXT AND THE WHOLE URL. This is the string that is on stream; abbreviating it is how
		   an operator fails to notice they are showing the wrong video. */
		m_shareState->setText(QStringLiteral("ON REACTIONS: %1").arg(*q.share));
		SetTone(m_shareState, QStringLiteral("go"));
	} else {
		m_shareState->setText(QStringLiteral("IDLE CARD"));
		SetTone(m_shareState, QString());
	}
	m_shareLoad->setEnabled(q.fresh.ever);
	m_shareClear->setEnabled(q.fresh.ever && q.share.has_value());

	/* ⚠️ THE COUNT IS IN THE SIGNATURE SO THAT AN EMPTY LIST HAS ONE. control.html hit this and
	   wrote it down: "seeded null for the same reason: an empty list's signature is '', so a ''
	   seed would make the first poll look unchanged and the EMPTY placeholder would never be
	   drawn." A default-constructed QString compares equal to an empty one, so without this the
	   very first render of an empty queue skips the rebuild and the pane shows nothing at all —
	   no rows and no "NOTHING WAITING", which reads as a pane that failed to load. */
	const QStringList roster = h.emotes ? *h.emotes : QStringList();
	const QString sig = QStringLiteral("n=%1;").arg(roster.size()) + roster.join(QLatin1Char('\x1f'));
	if (sig == m_rosterSig) {
		return;
	}
	m_rosterSig = sig;

	ClearLayout(m_roster);
	if (roster.isEmpty()) {
		/* ONE HONEST SENTENCE, NOT ZERO BUTTONS. The rig omits the roster rather than sending an
		   empty one precisely so this state is nameable: "drawing zero buttons and drawing no grid
		   at all are the same rig state and the panel should say so once." */
		m_roster->addWidget(EmptyRow(QStringLiteral("NO ROSTER — THE TANK HAS NOT ANSWERED")), 0, 0);
		return;
	}

	int row = 0;
	int col = 0;
	for (const QString &name : roster) {
		QPushButton *b = MakeButton(name, "qciRigEmote");
		connect(b, &QPushButton::clicked, this, [this, name]() { rig()->actEmote(name); });
		m_roster->addWidget(b, row, col);
		if (++col >= 3) {
			col = 0;
			row++;
		}
	}
}

/* ══ AUDIO ═════════════════════════════════════════════════════════════════════════════════════ */

QCiRigAudioPane::QCiRigAudioPane(QWidget *parent)
	: QCiRigPane(QCiRigClient::SurfaceAudio, QStringLiteral("AUDIO ROUTING"), parent)
{
	QWidget *summaryBox = new QWidget;
	QGridLayout *summary = new QGridLayout(summaryBox);
	summary->setContentsMargins(0, 0, 0, 0);
	summary->setSpacing(4);
	auto chip = [&](int row, const char *key, QLabel **out) {
		QLabel *k = MakeLabel(QString::fromUtf8(key), "qciRigChipKey");
		ApplyLabelFont(k, 120.0);
		*out = MakeLabel(QString::fromUtf8(ABSENT), "qciRigChipValue");
		(*out)->setWordWrap(true);
		summary->addWidget(k, row, 0);
		summary->addWidget(*out, row, 1);
	};
	chip(0, "MONITOR", &m_monitor);
	chip(1, "ADDED LATENCY", &m_latency);
	chip(2, "VIOLATIONS", &m_violations);
	chip(3, "TAPS VERIFIED", &m_taps);
	summary->setColumnStretch(1, 1);
	body()->addWidget(summaryBox);

	m_channelBox = new QWidget;
	m_channels = new QVBoxLayout(m_channelBox);
	m_channels->setContentsMargins(0, 0, 0, 0);
	m_channels->setSpacing(4);
	body()->addWidget(m_channelBox);

	QLabel *note = MakeLabel(QStringLiteral("LEVELS LIVE IN THE MIXER DOCK — THIS PANE IS THE ROUTING: "
						"WHAT IS GOING WHERE, OFF WHAT DEVICE, AT WHAT COST."),
				 "qciRigNote");
	note->setWordWrap(true);
	ApplyLabelFont(note, 116.0);
	body()->addWidget(note);

	body()->addStretch(1);

	renderModel(rig()->model());
}

void QCiRigAudioPane::renderModel(const Model &model)
{
	const Audio &a = model.audio;
	const bool fresh = applyFreshness(a.fresh);

	m_monitor->setText(a.monitor.isEmpty() ? QString::fromUtf8(ABSENT) : a.monitor);
	m_latency->setText(a.ok ? QStringLiteral("%1 ms").arg(a.addedLatencyMs) : QString::fromUtf8(ABSENT));
	SetTone(m_latency, a.ok && a.slackMs < 0 ? QStringLiteral("bad") : QString());

	if (!a.ok) {
		m_violations->setText(QString::fromUtf8(ABSENT));
		SetTone(m_violations, QStringLiteral("bad"));
	} else if (a.blocking > 0) {
		m_violations->setText(QStringLiteral("%1 BLOCKING").arg(a.blocking));
		SetTone(m_violations, QStringLiteral("bad"));
	} else if (a.warnings > 0) {
		m_violations->setText(QStringLiteral("%1 WARN").arg(a.warnings));
		SetTone(m_violations, QStringLiteral("warn"));
	} else {
		m_violations->setText(QStringLiteral("NONE"));
		SetTone(m_violations, QStringLiteral("go"));
	}

	/* ⚠️ `ok` AND `verified` ARE DIFFERENT QUESTIONS. An unprobed tap reports as unverified, and
	   that is the honest reading — "an ungranted tap is exit 0, healthy, and pure silence." The
	   count reads 0/2 on an ordinary poll, on purpose, because probing costs three seconds of real
	   sample measurement per device and cannot happen on a route polled every second. */
	int verified = 0;
	for (const Tap &t : a.taps) {
		if (t.verified) {
			verified++;
		}
	}
	m_taps->setText(a.taps.isEmpty() ? QStringLiteral("NO TAPS")
					 : QStringLiteral("%1/%2").arg(verified).arg(a.taps.size()));
	SetTone(m_taps, (!a.taps.isEmpty() && verified < a.taps.size()) ? QStringLiteral("warn") : QString());

	/* The mute state is IN the signature, which is what fixes each row's button direction at draw
	   time — see the note on the mask segment. A row that rebuilds when the mute changes is a row
	   whose button always says what it will do. */
	QString sig = QStringLiteral("n=%1;").arg(a.sources.size());
	for (const Channel &ch : a.sources) {
		sig += ch.name;
		sig += QLatin1Char(':');
		sig += ch.muted ? QLatin1Char('1') : QLatin1Char('0');
		sig += QLatin1Char(':');
		sig += ch.tone;
		sig += QLatin1Char(':');
		sig += ch.to.join(QLatin1Char('+'));
		sig += QLatin1Char('\x1f');
	}
	if (sig != m_channelSig) {
		m_channelSig = sig;
		ClearLayout(m_channels);
		if (a.sources.isEmpty()) {
			m_channels->addWidget(EmptyRow(a.live ? QStringLiteral("NO INPUTS IN THE ROUTING MODEL")
							      : QStringLiteral("OBS MIXER NOT SEEN YET")));
		} else {
			for (const Channel &ch : a.sources) {
				QFrame *row = MakeCard("qciRigRow");
				SetTone(row, ch.tone);
				QVBoxLayout *rowBox = new QVBoxLayout(row);
				rowBox->setContentsMargins(6, 4, 6, 4);
				rowBox->setSpacing(2);

				QWidget *topRow = new QWidget;
				QHBoxLayout *top = new QHBoxLayout(topRow);
				top->setContentsMargins(0, 0, 0, 0);
				top->setSpacing(6);
				QLabel *name = MakeLabel(ch.name, "qciRigCardWho");
				name->setTextFormat(Qt::PlainText);
				ApplyLabelFont(name, 112.0, true);
				top->addWidget(name, 1);

				/* NAMED DIRECTION, FIXED WHEN DRAWN. `muted` is in the signature above, so
				   this button is rebuilt whenever the state it inverts changes — which is
				   what makes "MUTE"/"UNMUTE" a promise rather than a guess about a document
				   that may be a second old. */
				const bool wantMuted = !ch.muted;
				QPushButton *mute = MakeButton(wantMuted ? QStringLiteral("MUTE")
									 : QStringLiteral("UNMUTE"),
							       ch.muted ? "qciRigUnmute" : "qciRigMute");
				const QString input = ch.name;
				connect(mute, &QPushButton::clicked, this,
					[this, input, wantMuted]() { rig()->actChannelMute(input, wantMuted); });
				mute->setEnabled(fresh);
				top->addWidget(mute);
				rowBox->addWidget(topRow);

				/* WHERE IT ACTUALLY ENDS UP, in the operator's words rather than OBS's enum,
				   derived by the rig from toStream/monitor rather than declared. */
				QStringList where;
				where << (ch.from.isEmpty() ? QStringLiteral("?") : ch.from);
				where << QStringLiteral("→");
				where << (ch.to.isEmpty() ? QStringLiteral("NOWHERE") : ch.to.join(QStringLiteral(" + ")));
				if (ch.alignMs != 0) {
					where << QStringLiteral("· %1 ms").arg(ch.alignMs);
				}
				if (ch.muted) {
					where << QStringLiteral("· MUTED");
				}
				QLabel *path = MakeLabel(where.join(QLatin1Char(' ')), "qciRigCardMeta");
				path->setWordWrap(true);
				rowBox->addWidget(path);

				if (!ch.note.isEmpty()) {
					QLabel *noteLabel = MakeLabel(ch.note, "qciRigNote");
					noteLabel->setWordWrap(true);
					noteLabel->setTextFormat(Qt::PlainText);
					rowBox->addWidget(noteLabel);
				}

				m_channels->addWidget(row);
			}
		}
	}

	if (fresh) {
		setStatus(a.live ? QStringLiteral("LIVE MIXER") : QStringLiteral("NO MIXER"),
			  a.live ? QString() : QStringLiteral("warn"));
	}
}
