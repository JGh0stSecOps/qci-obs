/******************************************************************************
    QCi Studio — the command bar. 30px, and OBS has nothing like it.

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

#include <QStringList>
#include <QWidget>

class QLabel;
class QLineEdit;
class QTimer;

/*
 * ══ THE COMMAND BAR ════════════════════════════════════════════════════════════════════════════
 *
 * The best thing on the 7" browser console is its command line, and OBS has nothing remotely like
 * it. Keeping it costs 30px at the bottom of the window and is the second element that makes this
 * application unmistakable before you read a label.
 *
 * ⚠️ THE STATUS READOUT IS LOAD-BEARING, NOT DECORATION. It is the sendOk() surface: every verb
 * prints either what it did or the SERVER'S OWN REFUSAL STRING. This is the fix for the class of
 * bug where /say printed "SAID" unconditionally and drew a green confirmation over silence. So the
 * readout is fed by QCiRigClient::say(), which is emitted from the reply, and never by the handler
 * that queued the request.
 *
 * ⚠️ AND THE VERB LIST IS THE WIRED ONE, NOT THE WISHED-FOR ONE. A completer that offers `light`
 * when nothing in this binary can drive the light bar is a completer that teaches the operator a
 * verb and then refuses it mid-stream. See VERBS in the .cpp for what is here and the list of what
 * is deliberately absent.
 */
class QCiCommandBar : public QWidget {
	Q_OBJECT

public:
	explicit QCiCommandBar(QWidget *parent = nullptr);

	/** Put the caret in the command line. Bound to ":" from anywhere in the window. */
	void focusCommandLine();

	/** Print a line in the readout. Tone is the rig's vocabulary — "bad", "warn", "go", or empty. */
	void report(const QString &text, const QString &tone = QString());

signals:
	/** `run` / `build`. The window owns the mode; this only asks. */
	void modeRequested(bool build);
	/** `queue` / `react` / `chat` / `tank` / `audio` — switch the operator section's page. */
	void pageRequested(const QString &page);

private:
	void submit();
	void dispatch(const QString &verb, const QString &rest);

	QLineEdit *m_line = nullptr;
	QLabel *m_out = nullptr;
	QTimer *m_revert = nullptr;
};
