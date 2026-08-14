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

#include "QCiSceneRail.hpp"

#include "QCiRigClient.hpp"
#include "QCiRigUi.hpp"

#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "moc_QCiSceneRail.cpp"

using namespace QCiRig;
using QCiUi::ApplyLabelFont;
using QCiUi::ClearLayout;
using QCiUi::MakeButton;
using QCiUi::MakeLabel;
using QCiUi::SetTone;

namespace {
/* 56px is the switcher row. It is not a taste number: it is the smallest row this operator can hit
   reliably without looking away from the camera, which is the entire job of this column. */
constexpr int ROW_H = 56;
constexpr int PANE_PAD = 8;
constexpr int ROW_GAP = 4;
} // namespace

QCiSceneRail::QCiSceneRail(QWidget *parent) : QWidget(parent)
{
	/* ⚠️ WITHOUT THIS THE THEME DOES NOT REACH THIS WIDGET AT ALL, and the failure is silent. Qt
	   honours a stylesheet background on a bare QWidget subclass only if the subclass reimplements
	   paintEvent() or carries WA_StyledBackground — otherwise QCi.obt's rule for this object name
	   parses, resolves, matches, and paints nothing. */
	setAttribute(Qt::WA_StyledBackground, true);
	setObjectName(QStringLiteral("qciSceneRail"));

	QVBoxLayout *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(0);

	/* THE RAIL SCROLLS AND NOTHING ELSE DOES. A collection can hold more program-safe scenes than
	   fit in a 1200px column, and the alternative — shrinking the rows until they all fit — throws
	   away the one property that makes this a switcher. */
	QScrollArea *scroll = new QScrollArea;
	scroll->setObjectName(QStringLiteral("qciSceneRailScroll"));
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	QWidget *column = new QWidget;
	column->setObjectName(QStringLiteral("qciSceneRailBody"));
	m_column = new QVBoxLayout(column);
	m_column->setContentsMargins(PANE_PAD, PANE_PAD, PANE_PAD, PANE_PAD);
	m_column->setSpacing(ROW_GAP);
	scroll->setWidget(column);
	outer->addWidget(scroll, 1);

	QCiRigClient *client = QCiRigClient::Get();
	/* THE ONLY SUBSCRIPTION THIS WIDGET MAKES. It declares which surface it draws and renders when
	   the model moves. It owns no network object, no timer, and no copy of the scene list. */
	client->wantSurface(QCiRigClient::SurfaceHub, this);
	connect(client, &QCiRigClient::changed, this, &QCiSceneRail::onModelChanged);

	onModelChanged();
}

void QCiSceneRail::activateRow(int index)
{
	/* NO WRAPPING AND NO CLAMPING. A key pressed into a rail that is shorter than the key's number
	   must do nothing — clamping to the last row would make the 9 key switch to whatever happens to
	   be at the bottom, which on this rig is the least-used scene and is exactly the sort of
	   surprise a live switcher must not produce. */
	if (index < 0 || index >= m_rows.size()) {
		return;
	}
	m_rows.at(index)->click();
}

void QCiSceneRail::onModelChanged()
{
	QCiRigClient *client = QCiRigClient::Get();
	const Model &model = client->model();
	const Hub &h = model.hub;

	rebuild(model);

	/* ── which one is on program ────────────────────────────────────────────────────────────────
	 * ⚠️ THE AUTHORITY IS OBS'S PROGRAM SCENE, NEVER /queue.lastScene. The console wrote that rule
	 * down after two pollers overwrote each other: lastScene is what was ASKED FOR — an INTENT —
	 * and /hub's `scene` is what GetCurrentProgramScene answered. A rail that lights the scene that
	 * was requested asserts a program the watchdog may already have overridden, which on a rig whose
	 * design rule is "never show green over a dead link" is the same class of lie. */
	const QString current = h.scene ? *h.scene : QString();
	for (QPushButton *row : m_rows) {
		const bool onProgram = !current.isEmpty() && row->property("qciSceneName").toString() == current;
		{
			QSignalBlocker block(row);
			row->setChecked(onProgram);
		}
		/* PROGRAM IS A TONE, NOT JUST A CHECK STATE, and the tone is `live` — the amber this whole
		   family already uses for on-air, on both firmwares and in the flight strip. The scene that
		   is currently on program is the one object in this column the eye must find without
		   reading. */
		SetTone(row, onProgram ? QStringLiteral("live") : QString());
	}

	/* ── freshness, three states ────────────────────────────────────────────────────────────────── */
	QString word;
	QString tone;
	if (!h.fresh.ever) {
		word = h.fresh.fault.isEmpty() ? QStringLiteral("NO LINK") : QStringLiteral("UNREACHABLE");
		tone = QStringLiteral("bad");
	} else if (!h.fresh.fault.isEmpty()) {
		word = QStringLiteral("STALE %1").arg(FormatAge(QCiRigClient::Get()->nowMs() - h.fresh.at));
		tone = QStringLiteral("warn");
	} else if (!h.scene) {
		word = QStringLiteral("NO PROGRAM");
		tone = QStringLiteral("bad");
	} else if (h.sceneOk && !*h.sceneOk) {
		/* A program scene the watchdog does not permit is a FAULT, not a preference: it is about to
		   be killed. The operator needs the noun rather than a vague red. */
		word = QStringLiteral("OFF ALLOWLIST");
		tone = QStringLiteral("bad");
	} else {
		word = QStringLiteral("%1 SCENES").arg(m_rows.size());
		tone = QStringLiteral("go");
	}
	if (word != m_freshness) {
		m_freshness = word;
		emit freshnessChanged(word, tone);
	}
}

void QCiSceneRail::rebuild(const Model &model)
{
	/* Signature over the REAL names, in order. The label is not in it: two scenes cannot share a
	   real name, so rebuilding because a label changed would be a rebuild for nothing.
	 *
	 * ⚠️ THE COUNT IS IN THE SIGNATURE SO THAT AN EMPTY LIST HAS ONE. The console hit this and wrote
	 * it down: an empty list's signature is "", and a default-constructed QString compares equal to
	 * an empty one — so without the count the very first render of an empty rail would look
	 * unchanged, the rebuild would be skipped, and the pane would show neither rows nor the
	 * placeholder. Which reads as a pane that failed to load.
	 *
	 * ⚠️ AND REBUILDING IS GATED FOR A SECOND REASON BEYOND COST: this rail is re-rendered once a
	 * second, and rebuilding it under the operator's cursor moves the button out from under a press
	 * that is already happening. On this column that press is a scene change on a live broadcast. */
	QString sig = QStringLiteral("n=%1;").arg(model.hub.scenes ? model.hub.scenes->size() : -1);
	if (model.hub.scenes) {
		for (const Scene &s : *model.hub.scenes) {
			sig += s.name;
			sig += QLatin1Char('\x1f');
		}
	}
	if (sig == m_sig) {
		return;
	}
	m_sig = sig;

	ClearLayout(m_column);
	m_rows.clear();
	m_empty = nullptr;

	if (!model.hub.scenes || model.hub.scenes->isEmpty()) {
		/* NO BUTTONS RATHER THAN DEAD BUTTONS, AND NEVER A COMPILED-IN LIST. See the header: the
		   one hand copy that ever existed went stale and offered a retired wrapper. */
		m_empty = MakeLabel(QStringLiteral("NO SCENES PUBLISHED — THE RIG CANNOT READ THE ALLOWLIST "
						   "OR OBS IS DOWN"),
				    "qciRigEmpty");
		m_empty->setAlignment(Qt::AlignCenter);
		m_empty->setWordWrap(true);
		ApplyLabelFont(m_empty, 118.0);
		m_column->addWidget(m_empty);
		m_column->addStretch(1);
		return;
	}

	for (const Scene &s : *model.hub.scenes) {
		QPushButton *row = MakeButton(s.label, "qciRailScene");
		row->setCheckable(true);
		row->setMinimumHeight(ROW_H);
		row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		ApplyLabelFont(row, 112.0, true);
		/* THE REAL NAME LIVES ON THE WIDGET, not in a parallel hash keyed by anything drawn. The
		   lambda captures it too; the property is what the program-scene comparison above reads, so
		   there is exactly one place a real name is stored per row. */
		const QString real = s.name;
		row->setProperty("qciSceneName", real);
		connect(row, &QPushButton::clicked, this, [real]() { QCiRigClient::Get()->actScene(real); });
		m_column->addWidget(row);
		m_rows.append(row);
	}
	m_column->addStretch(1);
}
