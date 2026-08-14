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

#include "QCiRigUi.hpp"

#include <QFont>
#include <QFrame>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QStyle>
#include <QWidget>

namespace QCiUi {

const char *const ABSENT = "—";

void SetTone(QWidget *w, const QString &tone)
{
	if (!w || w->property("qciTone").toString() == tone) {
		return;
	}
	w->setProperty("qciTone", tone);
	w->style()->unpolish(w);
	w->style()->polish(w);
}

void ApplyLabelFont(QWidget *w, qreal spacingPercent, bool bold)
{
	if (!w) {
		return;
	}
	QFont f = w->font();
	f.setCapitalization(QFont::AllUppercase);
	f.setLetterSpacing(QFont::PercentageSpacing, spacingPercent);
	f.setBold(bold);
	w->setFont(f);
}

void ApplyReadoutFont(QWidget *w, int pointSize, bool bold)
{
	if (!w) {
		return;
	}
	QFont f(QStringLiteral("Menlo"));
	f.setStyleHint(QFont::Monospace);
	f.setPointSize(pointSize);
	f.setBold(bold);
	/* ⚠️ NO AllUppercase HERE. A readout is digits and a colon; asking Qt to upper-case it costs a
	   shaping pass on every repaint of a clock that ticks once a second, and gains nothing. */
	w->setFont(f);
}

QLabel *MakeLabel(const QString &text, const char *objectName)
{
	QLabel *l = new QLabel(text);
	l->setObjectName(QString::fromUtf8(objectName));
	l->setTextInteractionFlags(Qt::NoTextInteraction);
	return l;
}

QPushButton *MakeButton(const QString &text, const char *objectName)
{
	QPushButton *b = new QPushButton(text);
	b->setObjectName(QString::fromUtf8(objectName));
	b->setCursor(Qt::PointingHandCursor);
	/* NO AUTO-DEFAULT. In a panel full of buttons, a Return keypress meant for a text field
	   otherwise lands on whichever button Qt decided was the default — and one of the buttons in
	   this application cuts the stream. */
	b->setAutoDefault(false);
	ApplyLabelFont(b, 108.0);
	return b;
}

QFrame *MakeCard(const char *objectName)
{
	QFrame *card = new QFrame;
	card->setObjectName(QString::fromUtf8(objectName));
	card->setFrameShape(QFrame::NoFrame);
	return card;
}

QFrame *MakeSeparator(int height)
{
	QFrame *sep = new QFrame;
	sep->setObjectName(QStringLiteral("qciSep"));
	sep->setFrameShape(QFrame::VLine);
	sep->setFrameShadow(QFrame::Plain);
	sep->setFixedSize(1, height);
	return sep;
}

void ClearLayout(QLayout *layout)
{
	if (!layout) {
		return;
	}
	while (QLayoutItem *item = layout->takeAt(0)) {
		if (QWidget *w = item->widget()) {
			/* HIDE FIRST, and it is not decoration: takeAt() removes the LAYOUT ITEM, not the
			   child, so the widget stays parented and painted at its old geometry until the
			   event loop gets round to the deleteLater(). The alternative idiom —
			   setParent(nullptr) — also hides it, by promoting it to a top-level window for
			   the interval, which on macOS is a real window being registered and torn down
			   several times a second while a queue is moving. */
			w->hide();
			w->deleteLater();
		}
		delete item;
	}
}

QString SceneLabelFor(const QString &realName)
{
	QString s = realName;
	const QString suffix = QStringLiteral(" (Delayed Output)");
	if (s.endsWith(suffix)) {
		s.chop(suffix.size());
	}
	return s;
}

} // namespace QCiUi
