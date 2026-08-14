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

#include "QCiCommandBar.hpp"

#include <docks/QCiRigClient.hpp>
#include <docks/QCiRigUi.hpp>

#include <QCompleter>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QStringListModel>
#include <QTimer>

#include "moc_QCiCommandBar.cpp"

using namespace QCiRig;
using QCiUi::ApplyLabelFont;
using QCiUi::ApplyReadoutFont;
using QCiUi::MakeLabel;
using QCiUi::SetTone;

namespace {

constexpr int BAR_HEIGHT = 30;
constexpr int OUT_WIDTH = 420;
constexpr int REVERT_MS = 3400;

/* ── THE VERBS THAT ACTUALLY DO SOMETHING IN THIS BINARY ─────────────────────────────────────────
 *
 * The layout spec lists 25. This is the subset QCiRigClient can currently drive, and the completer
 * offers exactly this list — no more.
 *
 * WHY NOT ALL 25. A completer is a promise: it teaches the operator that a word exists, and the
 * moment it exists in the operator's fingers it will be typed mid-stream. `light`, `tether`,
 * `bump`, `cmd`, `say`, `arm`, `brb`, `mirror`, `share` and `goal` have no verb on QCiRigClient —
 * there is no actLight(), no actArm(), no actShare(). Offering them would mean either a silent
 * no-op or a hand-rolled second HTTP path beside the one client, and this panel's whole rule is
 * that exactly one object talks to the rig.
 *
 * They are not forgotten and they are not "coming soon" in a comment: each needs a verb added to
 * QCiRigClient (which is where the single-in-flight, 1200ms-abort and 3-strike-backoff guards
 * live), and it joins this list on the same commit that adds it. That ordering is the point.
 */
struct Verb {
	const char *word;
	const char *help;
};

constexpr Verb VERBS[] = {
	{"hold", "PANIC. Switch to the hold scene, mask BLOCK, cut the virtual camera."},
	{"mask", "mask vision | mask block — a fixed direction, never a toggle."},
	{"scene", "scene <name> — the REAL scene name, as /hub publishes it."},
	{"golive", "golive on | golive off — a named direction."},
	{"go", "Hand off from the boot hold. The rig refuses it anywhere else."},
	{"emote", "emote <name> — from the roster the daemon publishes."},
	{"approve", "approve <id> | approve all"},
	{"reject", "reject <id>"},
	{"auto", "Flip the moderation AUTO master. No direction — the server owns it."},
	{"react", "Show the REACT page. react <url> also queues the link."},
	{"queue", "Show the QUEUE page."},
	{"chat", "Show the CHAT page."},
	{"tank", "Show the TANK page."},
	{"audio", "Show the AUDIO page."},
	{"run", "Leave the editor. The switcher console."},
	{"build", "The editor: scene and source trees, unlocked canvas."},
};

constexpr int VERB_N = int(sizeof(VERBS) / sizeof(VERBS[0]));

/* ⚠️ ESC BLURS, RETURN SUBMITS, AND NEITHER MAY REACH THE WINDOW. Without this filter, Escape in
   the command line propagates to the main window, and Return can land on whichever button Qt last
   decided was the default — one of which cuts the stream. */
class CommandLine : public QLineEdit {
public:
	using QLineEdit::QLineEdit;

protected:
	void keyPressEvent(QKeyEvent *event) override
	{
		if (event->key() == Qt::Key_Escape) {
			clearFocus();
			event->accept();
			return;
		}
		QLineEdit::keyPressEvent(event);
	}
};

} // namespace

QCiCommandBar::QCiCommandBar(QWidget *parent) : QWidget(parent)
{
	setAttribute(Qt::WA_StyledBackground, true);
	setObjectName(QStringLiteral("qciCommandBar"));
	setFixedHeight(BAR_HEIGHT);

	QHBoxLayout *row = new QHBoxLayout(this);
	row->setContentsMargins(12, 0, 12, 0);
	row->setSpacing(8);

	QLabel *prompt = MakeLabel(QStringLiteral(":"), "qciCmdPrompt");
	prompt->setFixedWidth(16);
	ApplyReadoutFont(prompt, 13, true);
	row->addWidget(prompt);

	m_line = new CommandLine;
	m_line->setObjectName(QStringLiteral("qciCmdLine"));
	m_line->setFrame(false);
	m_line->setPlaceholderText(QStringLiteral("TYPE : FOR COMMANDS"));
	ApplyReadoutFont(m_line, 13);
	row->addWidget(m_line, 1);

	QStringList words;
	for (int i = 0; i < VERB_N; i++) {
		words << QString::fromUtf8(VERBS[i].word);
	}
	QCompleter *completer = new QCompleter(words, this);
	completer->setCompletionMode(QCompleter::PopupCompletion);
	completer->setCaseSensitivity(Qt::CaseInsensitive);
	m_line->setCompleter(completer);

	QString tip = QStringLiteral("Verbs:\n");
	for (int i = 0; i < VERB_N; i++) {
		tip += QStringLiteral("  %1 — %2\n").arg(QString::fromUtf8(VERBS[i].word),
							 QString::fromUtf8(VERBS[i].help));
	}
	m_line->setToolTip(tip.trimmed());

	m_out = MakeLabel(QStringLiteral("READY"), "qciCmdOut");
	m_out->setFixedWidth(OUT_WIDTH);
	m_out->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
	ApplyLabelFont(m_out, 108.0);
	row->addWidget(m_out);

	connect(m_line, &QLineEdit::returnPressed, this, &QCiCommandBar::submit);

	m_revert = new QTimer(this);
	m_revert->setSingleShot(true);
	m_revert->setInterval(REVERT_MS);
	connect(m_revert, &QTimer::timeout, this, [this]() {
		m_out->setText(QStringLiteral("READY"));
		SetTone(m_out, QString());
	});

	/* ⚠️ THIS IS THE sendOk() SURFACE AND THIS CONNECTION IS THE WHOLE REASON IT IS HONEST. The
	   readout is fed from the REPLY, via QCiRigClient::say(), not from the handler that queued the
	   request. dispatch() below therefore prints only what it can know without asking anybody —
	   which page it switched to, or that a word is not a verb — and every rig-bound action leaves
	   the line blank until the server says something. */
	connect(QCiRigClient::Get(), &QCiRigClient::say, this, [this](const QString &text) {
		const bool bad = text.startsWith(QStringLiteral("REFUSED")) ||
				 text.contains(QStringLiteral("FAULT"));
		report(text, bad ? QStringLiteral("bad") : QString());
	});
}

void QCiCommandBar::focusCommandLine()
{
	m_line->setFocus(Qt::ShortcutFocusReason);
	m_line->selectAll();
}

void QCiCommandBar::report(const QString &text, const QString &tone)
{
	m_out->setText(text);
	SetTone(m_out, tone);
	m_revert->start();
}

void QCiCommandBar::submit()
{
	const QString raw = m_line->text().trimmed();
	if (raw.isEmpty()) {
		return;
	}
	const int space = raw.indexOf(QLatin1Char(' '));
	const QString verb = (space < 0 ? raw : raw.left(space)).toLower();
	const QString rest = space < 0 ? QString() : raw.mid(space + 1).trimmed();
	m_line->clear();
	dispatch(verb, rest);
}

void QCiCommandBar::dispatch(const QString &verb, const QString &rest)
{
	QCiRigClient *r = QCiRigClient::Get();

	/* ── the two window verbs. These this widget CAN confirm, because the window is in-process and
	     answers synchronously — so they are the only two that print their own success. */
	if (verb == QLatin1String("run") || verb == QLatin1String("build")) {
		const bool build = verb == QLatin1String("build");
		emit modeRequested(build);
		report(build ? QStringLiteral("BUILD") : QStringLiteral("RUN"), QStringLiteral("go"));
		return;
	}

	static const char *const PAGES[] = {"queue", "chat", "tank", "audio"};
	for (const char *page : PAGES) {
		if (verb == QLatin1String(page)) {
			emit pageRequested(verb);
			report(verb.toUpper(), QStringLiteral("go"));
			return;
		}
	}
	if (verb == QLatin1String("react")) {
		emit pageRequested(verb);
		if (rest.isEmpty()) {
			report(QStringLiteral("REACT"), QStringLiteral("go"));
		} else {
			/* The page switch is ours to confirm; the queueing is the rig's to confirm. */
			r->actReactionAdd(rest);
		}
		return;
	}

	/* ── the rig verbs. Every one of these goes quiet until the server answers. ───────────────── */
	if (verb == QLatin1String("hold")) {
		r->actHold();
		return;
	}
	if (verb == QLatin1String("mask")) {
		/* ⚠️ A DIRECTION IS REQUIRED AND `mask` ALONE DOES NOT TOGGLE. The rig refuses a mask
		   toggle by construction, and it names the reason: a toggle re-reads mask state at press
		   time from a document up to a second old, so the hand that meant "hide me" can un-hide
		   instead. A command line is not exempt from that. */
		if (rest != QLatin1String("vision") && rest != QLatin1String("block")) {
			report(QStringLiteral("MASK NEEDS A DIRECTION: MASK VISION OR MASK BLOCK"),
			       QStringLiteral("bad"));
			return;
		}
		r->actMask(rest);
		return;
	}
	if (verb == QLatin1String("scene")) {
		if (rest.isEmpty()) {
			report(QStringLiteral("SCENE NEEDS A NAME"), QStringLiteral("bad"));
			return;
		}
		/* ⚠️ SENT VERBATIM. The rig publishes a scene as a PAIR — `n` the real name, `l` the
		   printable one with " (Delayed Output)" stripped — and a client that sends the pretty
		   string asks for a scene that does not exist, which OBS answers by switching nothing,
		   silently. What the operator typed goes on the wire unaltered. */
		r->actScene(rest);
		return;
	}
	if (verb == QLatin1String("golive")) {
		if (rest != QLatin1String("on") && rest != QLatin1String("off")) {
			report(QStringLiteral("GOLIVE NEEDS A DIRECTION: GOLIVE ON OR GOLIVE OFF"),
			       QStringLiteral("bad"));
			return;
		}
		r->actStream(rest == QLatin1String("on"));
		return;
	}
	if (verb == QLatin1String("go")) {
		r->actGo();
		return;
	}
	if (verb == QLatin1String("emote")) {
		if (rest.isEmpty()) {
			report(QStringLiteral("EMOTE NEEDS A NAME"), QStringLiteral("bad"));
			return;
		}
		r->actEmote(rest);
		return;
	}
	if (verb == QLatin1String("auto")) {
		r->actAutoToggle();
		return;
	}
	if (verb == QLatin1String("approve") || verb == QLatin1String("reject")) {
		if (verb == QLatin1String("approve") && rest == QLatin1String("all")) {
			r->actApproveAll();
			return;
		}
		bool ok = false;
		const qint64 id = rest.toLongLong(&ok);
		if (!ok) {
			report(QStringLiteral("%1 NEEDS AN EVENT ID").arg(verb.toUpper()), QStringLiteral("bad"));
			return;
		}
		if (verb == QLatin1String("approve")) {
			r->actApprove(id);
		} else {
			r->actReject(id);
		}
		return;
	}

	report(QStringLiteral("NOT A VERB: %1").arg(verb.toUpper()), QStringLiteral("bad"));
}
