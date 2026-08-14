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

#pragma once

#include <obs.hpp>
/* Included in the HEADER rather than the .cpp because onFrontendEvent() takes an
   obs_frontend_event by value, and C++ forbids a forward reference to an enum type. */
#include <obs-frontend-api.h>

#include <QColor>
#include <QHash>
#include <QMutex>
#include <QString>
#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QTimer;
class QVBoxLayout;

/*
 * ══ THE LED METER ══════════════════════════════════════════════════════════════════════════════
 *
 * ⚠️ THIS IS NOT A THEMED VolumeMeter AND IT COULD NOT BE. data/themes/QCi.obt already states the
 * reason and it is measured, not assumed: VolumeMeter paints a CONTINUOUS bar in C++ and only its
 * colours are reachable from a .obt. Every level field it draws from — displayPeak, displayMagnitude,
 * displayPeakHold — is private and it has no friend but VolumeControl, so a subclass cannot repaint
 * them either. A discrete cell meter is therefore its own widget attached to its own
 * obs_volmeter_t, which is about a hundred lines and owes nothing to the stock one.
 *
 * ⚠️ THE CLASS-FLIP RUNS AT 340ms HOLDING PEAK-SINCE-LAST-FLIP. 2.94Hz, deliberately below the
 * photosensitivity flash band, and deliberately NOT a repaint-on-every-callback: the volmeter
 * callback arrives on the AUDIO thread many times a second, and turning each one into a widget
 * repaint would put a Qt paint on the audio path of a machine that is encoding a live stream.
 * So the callback does nothing but stash a peak under a mutex, and a QTimer in the GUI thread
 * decides when a cell changes colour.
 */
class QCiLedMeter : public QWidget {
	Q_OBJECT

	/* The four cell colours, as Qt properties, so QCi.obt owns them and no C++ file here names a
	   hex. Same contract as the qciTone protocol the panes use. */
	Q_PROPERTY(QColor cellUnlitColor MEMBER m_unlit DESIGNABLE true)
	Q_PROPERTY(QColor cellLitColor MEMBER m_lit DESIGNABLE true)
	Q_PROPERTY(QColor cellHotColor MEMBER m_hot DESIGNABLE true)
	Q_PROPERTY(QColor cellPeakColor MEMBER m_peak DESIGNABLE true)

public:
	explicit QCiLedMeter(QWidget *parent = nullptr);
	~QCiLedMeter() override;

	void attach(obs_source_t *source);

	QSize minimumSizeHint() const override;
	QSize sizeHint() const override;

signals:
	/** The peak the meter is currently DRAWING, on the same 340ms tick, so the number beside the
	 *  meter and the lit cells can never describe two different instants. A separate timer for the
	 *  dB label would let them disagree, which on a clip is exactly when it matters. */
	void levelChanged(float peakDb, bool attached);

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	static void onLevels(void *param, const float magnitude[MAX_AUDIO_CHANNELS],
			     const float peak[MAX_AUDIO_CHANNELS], const float inputPeak[MAX_AUDIO_CHANNELS]);
	void flip();

	OBSVolMeter m_volmeter;
	bool m_attached = false;

	QMutex m_mutex;
	/* Written on the audio thread, read on the GUI thread, both under m_mutex. Nothing else is
	   shared between the two. */
	float m_pendingPeak = -100.0f;

	/* Read and written only on the GUI thread. */
	float m_shownPeak = -100.0f;
	QTimer *m_tick = nullptr;

	QColor m_unlit;
	QColor m_lit;
	QColor m_hot;
	QColor m_peak;
};

/*
 * ══ THE AUDIO STRIP ════════════════════════════════════════════════════════════════════════════
 *
 * A permanent horizontal band across the bottom of RUN mode, because audio is the failure mode a
 * streamer cannot see and a dock you have to tab to is a dock you find out about afterwards.
 *
 * ⚠️ IT IS NOT THE STOCK MIXER AND IT IS NOT A SECOND COPY OF IT. The stock mixer is a scroll of
 * VolumeControl rows with per-source gear menus and a context menu into Advanced Audio Properties —
 * a production tool, and the place the JBL/Shokz routing is actually BUILT. That stays, in BUILD
 * mode, where routing is constructed. This is the OPERATING surface: mute, level, and where the
 * audio ends up.
 *
 * ⚠️ MON IS THREE STATES AND STAYS THREE STATES. "Let me hear the chat" and "put the chat in the
 * broadcast" are different requests, and OBS_MONITORING_TYPE_MONITOR_ONLY is what makes the
 * invasion routing possible at all. The labels say where the audio ENDS UP rather than naming the
 * enum, because the enum's own names ("monitor and output") do not answer the operator's question.
 */
class QCiAudioStrip : public QWidget {
	Q_OBJECT

public:
	explicit QCiAudioStrip(QWidget *parent = nullptr);
	~QCiAudioStrip() override;

	/** 26 + 34*channels, clamped [120,200]. The dock asks for this when it lays itself out. */
	int preferredHeight() const;

signals:
	void channelsChanged(const QString &word, const QString &tone);

private:
	struct Row {
		QString uuid;
		QWidget *widget = nullptr;
		QLabel *name = nullptr;
		QCiLedMeter *meter = nullptr;
		QPushButton *mute = nullptr;
		QComboBox *mon = nullptr;
	};

	void rebuild();
	void refreshStates();
	static void onFrontendEvent(enum obs_frontend_event event, void *data);

	QLabel *m_header = nullptr;
	QVBoxLayout *m_rowsLayout = nullptr;
	QList<Row> m_rows;
	QString m_sig;
	QTimer *m_stateTimer = nullptr;
};
