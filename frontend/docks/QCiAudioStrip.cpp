/******************************************************************************
    QCi Studio — the audio strip. The operating surface, not the building one.

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

#include "QCiAudioStrip.hpp"

#include "QCiRigUi.hpp"

#include <qt-wrappers.hpp>
#include <util/config-file.h>

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMutexLocker>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "moc_QCiAudioStrip.cpp"

using QCiUi::ApplyLabelFont;
using QCiUi::ApplyReadoutFont;
using QCiUi::ClearLayout;
using QCiUi::MakeButton;
using QCiUi::MakeLabel;
using QCiUi::SetTone;

namespace {

/* 18 cells, 2px gaps. The top 4 are "hot", the top 1 is peak/clip. */
constexpr int CELLS = 18;
constexpr int CELL_GAP = 2;
constexpr int HOT_CELLS = 4;
/* dBFS at the bottom of the meter. Below this the meter is dark rather than showing one lit cell
   for room tone, which is the reading an operator actually wants: "is anything arriving". */
constexpr float FLOOR_DB = -60.0f;
/* 2.94Hz. See the header for why this is a timer and not a repaint-per-callback. */
constexpr int FLIP_MS = 340;

constexpr int HEADER_H = 26;
constexpr int ROW_H = 34;
constexpr int NAME_W = 140;
constexpr int DB_W = 64;
constexpr int MUTE_W = 72;
constexpr int MON_W = 150;
constexpr int MIN_H = 120;
constexpr int MAX_H = 200;

} // namespace

/* ══ the meter ═════════════════════════════════════════════════════════════════════════════════ */

QCiLedMeter::QCiLedMeter(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("qciLedMeter"));
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

	/* OBS_FADER_LOG, the same curve the stock meter uses, so a level read here and a level read in
	   the BUILD-mode mixer two docks away describe the same sound. */
	m_volmeter = obs_volmeter_create(OBS_FADER_LOG);
	obs_volmeter_add_callback(m_volmeter, QCiLedMeter::onLevels, this);

	m_tick = new QTimer(this);
	m_tick->setInterval(FLIP_MS);
	connect(m_tick, &QTimer::timeout, this, &QCiLedMeter::flip);
	m_tick->start();
}

QCiLedMeter::~QCiLedMeter()
{
	/* ⚠️ REMOVE THE CALLBACK BEFORE THE OBJECT DIES. The volmeter dispatches on the audio thread,
	   so a callback still registered against a half-destroyed widget is a use-after-free with the
	   worst possible reproduction rate. */
	obs_volmeter_remove_callback(m_volmeter, QCiLedMeter::onLevels, this);
}

void QCiLedMeter::attach(obs_source_t *source)
{
	m_attached = source && obs_volmeter_attach_source(m_volmeter, source);
	if (!m_attached) {
		QMutexLocker lock(&m_mutex);
		m_pendingPeak = -100.0f;
	}
}

void QCiLedMeter::onLevels(void *param, const float magnitude[MAX_AUDIO_CHANNELS],
			   const float peak[MAX_AUDIO_CHANNELS], const float inputPeak[MAX_AUDIO_CHANNELS])
{
	UNUSED_PARAMETER(magnitude);
	UNUSED_PARAMETER(inputPeak);

	QCiLedMeter *self = static_cast<QCiLedMeter *>(param);

	/* ⚠️ THIS RUNS ON THE AUDIO THREAD. It touches ONE float under ONE mutex and does nothing else
	   — no Qt call, no repaint, no signal. A widget update from here would put Qt's paint machinery
	   on the audio path of a machine that is encoding a live broadcast. */
	float loudest = -100.0f;
	for (int i = 0; i < MAX_AUDIO_CHANNELS; i++) {
		if (std::isfinite(peak[i]) && peak[i] > loudest) {
			loudest = peak[i];
		}
	}

	QMutexLocker lock(&self->m_mutex);
	/* PEAK SINCE THE LAST FLIP, not the instantaneous value. Sampling at 2.94Hz without holding the
	   peak would miss transients entirely — the meter would sit dark through a clip. */
	if (loudest > self->m_pendingPeak) {
		self->m_pendingPeak = loudest;
	}
}

void QCiLedMeter::flip()
{
	float taken;
	{
		QMutexLocker lock(&m_mutex);
		taken = m_pendingPeak;
		m_pendingPeak = -100.0f;
	}
	if (std::abs(taken - m_shownPeak) < 0.01f) {
		return;
	}
	m_shownPeak = taken;
	emit levelChanged(m_shownPeak, m_attached);
	update();
}

QSize QCiLedMeter::minimumSizeHint() const
{
	return QSize(CELLS * 3, 10);
}

QSize QCiLedMeter::sizeHint() const
{
	return QSize(240, 12);
}

void QCiLedMeter::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	const int w = width();
	const int h = height();
	if (w <= 0 || h <= 0) {
		return;
	}

	const int cellW = std::max(1, (w - CELL_GAP * (CELLS - 1)) / CELLS);
	/* How many cells the level lights, floored at zero. A source with no audio arriving lights
	   nothing at all rather than one cell, because "nothing is arriving" is a reading. */
	int litCount = 0;
	if (m_attached && m_shownPeak > FLOOR_DB) {
		const float frac = (m_shownPeak - FLOOR_DB) / (0.0f - FLOOR_DB);
		litCount = std::clamp(int(std::ceil(frac * CELLS)), 0, CELLS);
	}

	for (int i = 0; i < CELLS; i++) {
		const int x = i * (cellW + CELL_GAP);
		QColor c = m_unlit;
		if (i < litCount) {
			if (i == CELLS - 1) {
				c = m_peak;
			} else if (i >= CELLS - HOT_CELLS) {
				c = m_hot;
			} else {
				c = m_lit;
			}
		}
		/* A colour the theme has not set yet is not painted at all. Falling back to a hard-coded
		   green here would put a hex in this file, which is exactly what the qciTone protocol and
		   the palette gate exist to prevent. */
		if (!c.isValid()) {
			continue;
		}
		p.fillRect(x, 0, cellW, h, c);
	}
}

/* ══ the strip ═════════════════════════════════════════════════════════════════════════════════ */

QCiAudioStrip::QCiAudioStrip(QWidget *parent) : QWidget(parent)
{
	setAttribute(Qt::WA_StyledBackground, true);
	setObjectName(QStringLiteral("qciAudioStrip"));

	QVBoxLayout *outer = new QVBoxLayout(this);
	outer->setContentsMargins(8, 0, 8, 8);
	outer->setSpacing(0);

	m_header = MakeLabel(QString(), "qciAudioStripHead");
	m_header->setFixedHeight(HEADER_H);
	ApplyLabelFont(m_header, 110.0);
	outer->addWidget(m_header);

	QWidget *rows = new QWidget;
	m_rowsLayout = new QVBoxLayout(rows);
	m_rowsLayout->setContentsMargins(0, 0, 0, 0);
	m_rowsLayout->setSpacing(0);
	outer->addWidget(rows, 1);

	obs_frontend_add_event_callback(QCiAudioStrip::onFrontendEvent, this);

	/* ⚠️ MUTE AND MONITORING ARE CHANGED FROM FOUR PLACES — this strip, the BUILD-mode mixer,
	   Advanced Audio Properties, and the 7" panel through the events server — and libobs emits no
	   frontend event for either. So the row states are re-read on a slow timer rather than assumed
	   from what was last pressed here. 800ms is the same cadence the console reconciles at, and it
	   is cheap: it reads two getters per row and assigns two widget states.

	   This is NOT a poll of the rig. It is a read of this process's own audio graph, which is the
	   authority for these two facts — the app owns the mixer. */
	m_stateTimer = new QTimer(this);
	m_stateTimer->setInterval(800);
	connect(m_stateTimer, &QTimer::timeout, this, &QCiAudioStrip::refreshStates);
	m_stateTimer->start();

	rebuild();
}

QCiAudioStrip::~QCiAudioStrip()
{
	obs_frontend_remove_event_callback(QCiAudioStrip::onFrontendEvent, this);
}

void QCiAudioStrip::onFrontendEvent(enum obs_frontend_event event, void *data)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		/* QUEUED, ALWAYS. Frontend events arrive from libobs's own thread and rebuild() deletes
		   widgets; doing that anywhere but the GUI thread is a crash waiting for a scene
		   collection switch. */
		QMetaObject::invokeMethod(static_cast<QCiAudioStrip *>(data), [data]() {
			static_cast<QCiAudioStrip *>(data)->rebuild();
		}, Qt::QueuedConnection);
		break;
	default:
		break;
	}
}

int QCiAudioStrip::preferredHeight() const
{
	return std::clamp(HEADER_H + ROW_H * int(m_rows.size()) + 8, MIN_H, MAX_H);
}

void QCiAudioStrip::rebuild()
{
	/* The channel set, as libobs has it right now. Global audio devices (desktop/mic, channels
	   1..6) plus any scene source that actually carries audio. */
	struct Found {
		QString uuid;
		QString name;
	};
	QList<Found> found;
	QStringList seen;

	struct EnumCtx {
		QList<Found> *found;
		QStringList *seen;
	};
	EnumCtx ctx{&found, &seen};

	auto consider = [](EnumCtx *c, obs_source_t *s) {
		if (!s || !(obs_source_get_output_flags(s) & OBS_SOURCE_AUDIO)) {
			return;
		}
		const char *uuidRaw = obs_source_get_uuid(s);
		const QString uuid = uuidRaw ? QString::fromUtf8(uuidRaw) : QString();
		/* ⚠️ THE GLOBAL DEVICES AND THE SCENE SOURCES OVERLAP. A mic wired into channel 1 is
		   also enumerable, and two rows for one source would mean two volmeters on it and two
		   MUTE buttons that disagree the instant one is pressed. */
		if (uuid.isEmpty() || c->seen->contains(uuid)) {
			return;
		}
		*c->seen << uuid;
		c->found->append({uuid, QString::fromUtf8(obs_source_get_name(s))});
	};

	/* Channels 1..6 first, so the desktop and mic devices head the strip in the order the mixer and
	   the rig both use, rather than in whatever order obs_enum_sources happens to walk. */
	for (uint32_t ch = 1; ch <= 6; ch++) {
		OBSSourceAutoRelease s = obs_get_output_source(ch);
		consider(&ctx, s);
	}

	using ConsiderFn = void (*)(EnumCtx *, obs_source_t *);
	struct WalkCtx {
		EnumCtx *ctx;
		ConsiderFn fn;
	};
	WalkCtx walk{&ctx, consider};
	obs_enum_sources(
		[](void *param, obs_source_t *s) {
			WalkCtx *w = static_cast<WalkCtx *>(param);
			if (obs_source_audio_active(s)) {
				w->fn(w->ctx, s);
			}
			return true;
		},
		&walk);

	QString sig = QStringLiteral("n=%1;").arg(found.size());
	for (const Found &f : found) {
		sig += f.uuid;
		sig += QLatin1Char('\x1f');
	}
	if (sig == m_sig) {
		return;
	}
	m_sig = sig;

	ClearLayout(m_rowsLayout);
	m_rows.clear();

	if (found.isEmpty()) {
		QLabel *empty = MakeLabel(QStringLiteral("NO AUDIO SOURCES"), "qciAudioStripEmpty");
		empty->setAlignment(Qt::AlignCenter);
		ApplyLabelFont(empty, 118.0);
		SetTone(empty, QStringLiteral("bad"));
		m_rowsLayout->addWidget(empty);
		emit channelsChanged(QStringLiteral("NO CHANNELS"), QStringLiteral("bad"));
		return;
	}

	for (const Found &f : found) {
		Row row;
		row.uuid = f.uuid;

		QWidget *w = new QWidget;
		w->setObjectName(QStringLiteral("qciAudioRow"));
		w->setAttribute(Qt::WA_StyledBackground, true);
		w->setFixedHeight(ROW_H);
		QHBoxLayout *r = new QHBoxLayout(w);
		r->setContentsMargins(8, 6, 8, 6);
		r->setSpacing(8);

		row.name = MakeLabel(f.name, "qciAudioName");
		row.name->setFixedWidth(NAME_W);
		row.name->setToolTip(f.name);
		ApplyLabelFont(row.name, 108.0);
		r->addWidget(row.name);

		row.meter = new QCiLedMeter;
		r->addWidget(row.meter, 1);

		QLabel *db = MakeLabel(QString::fromUtf8(QCiUi::ABSENT), "qciAudioDb");
		db->setFixedWidth(DB_W);
		db->setAlignment(Qt::AlignCenter);
		ApplyReadoutFont(db, 13);
		r->addWidget(db);

		/* An unattached meter, or a level under the floor, prints ABSENT rather than "-60.0".
		   A number is a measurement; a floor value is the meter saying it heard nothing, and
		   those read identically once they are both digits. */
		connect(row.meter, &QCiLedMeter::levelChanged, db, [db](float peakDb, bool attached) {
			if (!attached || peakDb <= FLOOR_DB) {
				db->setText(QString::fromUtf8(QCiUi::ABSENT));
				SetTone(db, QString());
				return;
			}
			db->setText(QString::number(peakDb, 'f', 1));
			SetTone(db, peakDb >= -1.0f ? QStringLiteral("bad") : QString());
		});

		row.mute = MakeButton(QStringLiteral("MUTE"), "qciAudioMute");
		row.mute->setCheckable(true);
		row.mute->setFixedWidth(MUTE_W);
		r->addWidget(row.mute);

		row.mon = new QComboBox;
		row.mon->setObjectName(QStringLiteral("qciAudioMon"));
		row.mon->setFixedWidth(MON_W);
		ApplyLabelFont(row.mon, 108.0);
		/* THE LABELS SAY WHERE THE AUDIO ENDS UP. libobs's own names for these — "monitor off",
		   "monitor only", "monitor and output" — describe the enum rather than the outcome, and
		   the outcome is the thing the operator is deciding between. */
		row.mon->addItem(QStringLiteral("MON OFF"), int(OBS_MONITORING_TYPE_NONE));
		row.mon->addItem(QStringLiteral("EARS ONLY"), int(OBS_MONITORING_TYPE_MONITOR_ONLY));
		row.mon->addItem(QStringLiteral("EARS + STREAM"), int(OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT));
		r->addWidget(row.mon);

		const QString uuid = f.uuid;
		connect(row.mute, &QPushButton::clicked, this, [uuid](bool checked) {
			OBSSourceAutoRelease s = obs_get_source_by_uuid(QT_TO_UTF8(uuid));
			if (s) {
				obs_source_set_muted(s, checked);
			}
		});
		connect(row.mon, &QComboBox::activated, this, [uuid, combo = row.mon](int index) {
			OBSSourceAutoRelease s = obs_get_source_by_uuid(QT_TO_UTF8(uuid));
			if (s) {
				obs_source_set_monitoring_type(
					s, obs_monitoring_type(combo->itemData(index).toInt()));
			}
		});

		OBSSourceAutoRelease src = obs_get_source_by_uuid(QT_TO_UTF8(uuid));
		row.meter->attach(src);

		row.widget = w;
		m_rowsLayout->addWidget(w);
		m_rows.append(row);
	}
	m_rowsLayout->addStretch(1);

	/* The monitoring device is a profile setting, not a per-source one, and naming it here is the
	   difference between "EARS ONLY" meaning something and meaning nothing. */
	const char *monDevice = config_get_string(obs_frontend_get_profile_config(), "Audio", "MonitoringDeviceName");
	m_header->setText(QStringLiteral("%1 CH · MON BUS %2")
				  .arg(m_rows.size())
				  .arg(monDevice && *monDevice ? QString::fromUtf8(monDevice)
							       : QString::fromUtf8(QCiUi::ABSENT)));

	emit channelsChanged(QStringLiteral("%1 CH").arg(m_rows.size()), QString());
	refreshStates();
}

void QCiAudioStrip::refreshStates()
{
	for (Row &row : m_rows) {
		OBSSourceAutoRelease s = obs_get_source_by_uuid(QT_TO_UTF8(row.uuid));
		if (!s) {
			/* The source went away between the rebuild and now — a scene collection change is
			   in flight. Say nothing rather than drawing a stale mute state; the queued
			   rebuild is already on its way. */
			row.mute->setEnabled(false);
			row.mon->setEnabled(false);
			continue;
		}
		row.mute->setEnabled(true);
		row.mon->setEnabled(true);

		const bool muted = obs_source_muted(s);
		if (row.mute->isChecked() != muted) {
			QSignalBlocker block(row.mute);
			row.mute->setChecked(muted);
		}
		SetTone(row.mute, muted ? QStringLiteral("bad") : QString());

		const int type = int(obs_source_get_monitoring_type(s));
		const int index = row.mon->findData(type);
		if (index >= 0 && row.mon->currentIndex() != index) {
			QSignalBlocker block(row.mon);
			row.mon->setCurrentIndex(index);
		}
	}
}
