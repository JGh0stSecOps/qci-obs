/******************************************************************************
    QCi Studio — the small vocabulary every QCi-authored widget is built from.

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

#include <QString>

class QFrame;
class QLabel;
class QLayout;
class QPushButton;
class QWidget;

/*
 * ══ THE HOUSE VOCABULARY ═══════════════════════════════════════════════════════════════════════
 *
 * These five functions lived in an anonymous namespace inside QCiRigPanes.cpp, where exactly one
 * translation unit could reach them. The flight strip, the command bar, the scene rail, the audio
 * strip and the operator section are five MORE surfaces that have to look like the same
 * application, and the way five surfaces stop looking like one application is each one growing its
 * own private copy of "how a label is drawn here".
 *
 * So they are promoted, not copied. QCiRigPanes.cpp now calls these.
 */
namespace QCiUi {

/* ── THE ONE PLACE A TONE BECOMES A LOOK ─────────────────────────────────────────────────────────
 * The rig speaks in tones — "bad", "warn", "go", "live", and nothing — and the theme decides what
 * those are worth in colour. NOTHING that calls this names a hex: it sets a dynamic property and
 * re-polishes, so data/themes/QCi.obt owns every colour in the application.
 *
 * The unpolish/polish pair is not optional. Qt resolves property selectors when a widget is
 * polished; changing the property afterwards leaves the old rule in force, which shows up as a
 * fault indicator that lit once and then never went out. */
void SetTone(QWidget *w, const QString &tone);

/* ⚠️ UPPERCASE AND LETTER-SPACING ARE A QFont JOB, NOT A STYLESHEET ONE, and QCi.obt says so in its
 * own header: "Qt QSS supports neither text-transform nor letter-spacing, and the rig's headings
 * are both." This is that font. It is the single largest reason a QCi surface reads as an
 * instrument rather than as stock OBS, and it is why it is in a shared header rather than in one
 * .cpp: a surface that forgets to call it is a surface that reverts to Qt's default look. */
void ApplyLabelFont(QWidget *w, qreal spacingPercent, bool bold = false);

/* The numeric face. Every number in the application — dB, telemetry, counters, timecode, uptime,
 * money — is Menlo, because a column of proportional digits does not line up and a readout that
 * does not line up cannot be read at a glance. Menlo is at /System/Library/Fonts/Menlo.ttc and is
 * OS-guaranteed on macOS; the family name is passed through so a missing face falls back to Qt's
 * own monospace rather than to the label face. */
void ApplyReadoutFont(QWidget *w, int pointSize, bool bold = false);

QLabel *MakeLabel(const QString &text, const char *objectName);
QPushButton *MakeButton(const QString &text, const char *objectName);
QFrame *MakeCard(const char *objectName);

/** A vertical hairline, for separating groups inside a horizontal strip. */
QFrame *MakeSeparator(int height);

/** Empty a layout, destroying the widgets in it. Used only on the signature-diff path — rebuilding
 *  a list under the operator's cursor moves the button out from under a press already happening. */
void ClearLayout(QLayout *layout);

/** A value the rig did not publish. ONE spelling, so an absent number never looks like a measured
 *  one and never looks like two different problems. */
extern const char *const ABSENT;

/** The printable form of a scene name. The rig publishes `n` (real) and `l` (label) as a PAIR and
 *  every route that carries them restates that the label has had " (Delayed Output)" stripped —
 *  this is the fallback for the one place the fork holds only a real name (OBS's own program scene,
 *  read out of libobs rather than off the wire) and still has to print it. It is DISPLAY ONLY.
 *  Nothing may ever send the result of this function back to the rig. */
QString SceneLabelFor(const QString &realName);

} // namespace QCiUi
