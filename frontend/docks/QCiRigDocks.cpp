/******************************************************************************
    QCi Studio — native docks for the QCi rig operator panel.

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

#include "QCiRigDocks.hpp"
#include "QCiDock.hpp"
#include "QCiRigClient.hpp"
#include "QCiRigPanes.hpp"

#include <widgets/QCiBasic.hpp>

#include <qt-wrappers.hpp>

#include <QAction>
#include <QCloseEvent>
#include <QDockWidget>
#include <QGuiApplication>
#include <QHash>
#include <QHostAddress>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QSize>
#include <QTimer>
#include <QUrl>

#include <algorithm>

namespace QCiRigDocks {

/* ─────────────────────────────────────────────────────────────────────────────────────────────
 * THE PANEL'S ADDRESS IS STATED ONCE, HERE, AND NOWHERE ELSE IN THIS FRONTEND.
 *
 * The rig repo spent its whole life deleting duplicated endpoints — the OBS websocket sat in five
 * files, the tank's address in four, the password in four with four different fallbacks — and
 * every one of those was a fact that had been RETYPED rather than derived. qci-rig/lib/config.mjs
 * is the home of this one (`EVENTS_API`), and it publishes three ways out: node imports it, zsh
 * and python ask `node lib/config.mjs get`, and browsers read `GET /config` off the server itself.
 *
 * A compiled binary in a different repository can use none of those before it knows where the
 * server is. So this constant is the BOOTSTRAP DEFAULT and nothing more: it is overridden by the
 * runtime setting below, it is read through exactly one accessor, and qci-rig's
 * lib/studiodocks.test.mjs asserts it is character-for-character equal to config.EVENTS_API and
 * that no other file under frontend/ spells the address at all.
 *
 * If you are about to paste this URL somewhere else: call BaseUrl(). QCiRigClient does.
 */
static constexpr const char *QCI_RIG_BASE_URL_DEFAULT = "http://127.0.0.1:8778";

/* Runtime home of the setting: ~/Library/Application Support/qci-studio/user.ini
 *
 *     [QCiRig]
 *     BaseUrl=http://127.0.0.1:8778
 *     DocksPlaced=true
 *
 * Editable from the UI at Docks → "QCi Rig Panel URL…" (PromptForBaseUrl below). It is USER config
 * rather than profile config on purpose: the rig's events server is a property of this machine,
 * not of a streaming profile, and an operator who switches profiles mid-stream must not have the
 * docks silently point somewhere else. */
static constexpr const char *CFG_SECTION = "QCiRig";
static constexpr const char *CFG_BASE_URL = "BaseUrl";
static constexpr const char *CFG_DOCKS_PLACED = "DocksPlaced";

/* ─────────────────────────────────────────────────────────────────────────────────────────────
 * THE DOCK TABLE.
 *
 * `pane` is the native widget class MakeRigDockContent() builds. `spec` is the provenance: which
 * region(s) of qci-rig/events/control.html this pane reproduces, from that page's own maps —
 *
 *     TAB_REGION  queue rB1 · share rC3 · goal rB2 · program rA1 · chat rC1
 *                 audio rC2 · control rA2 · transport rA3 · reactions rB3
 *     TAB_MODAL   tank/emotes · cam/camfx · mic/micfx · phone · scene · start · streams
 *                 policy · light · keys
 *
 * ⚠️ THE CONTROL PANE CARRIES TWO OF THOSE REGIONS, AND THAT IS WHY `spec` IS A LIST. The console
 * splits CONTROL (rA2: scene, mask, hold) from TRANSPORT (rA3: go live, the countdown arm) because
 * a 720x1280 panel shows one tab at a time and those are two tabs' worth of height. A dock column
 * has no such constraint and every one of those controls is in the same "act now" class, so the
 * native pane is one pane — and it therefore inherits rA3's safety marking as well as rA2's.
 *
 * ⚠️ ADDING A ROW IS A SAFETY DECISION. Read RigDockGroup in the header and Tabifiable() below
 * before you set the last two fields. A row that is tabified is a row that can be INVISIBLE.
 */
/* clang-format off */
static constexpr RigDock RIG_DOCKS[] = {
	/* CONTROL — PRIVACY HOLD, mask mode, the scene picker, the stream transport and the vitals.
	   Never tabified AND never closable: this is the pane the panic control lives on, and a
	   privacy control you cannot see is a privacy control you cannot verify. "Did I leave it on
	   VISION?" is not a question that should require clicking a tab to answer. */
	{"qciRigControlDock", "QCiRigControlPane", "control transport", "QCiRig.Dock.Control",
	 Qt::RightDockWidgetArea, true, true, GROUP_NEVER},
	/* The audience-facing panes. These share one tab bar, which is the whole point: each dock
	   burns ~62-66px on its own title bar, and five of them stacked would spend a third of a
	   1080p side column on chrome. Nothing here is dangerous when hidden — a queue you are not
	   looking at is a queue, not a live microphone. */
	{"qciRigQueueDock", "QCiRigQueuePane", "queue", "QCiRig.Dock.Queue",
	 Qt::RightDockWidgetArea, true, false, GROUP_PANEL},
	{"qciRigReactionsDock", "QCiRigReactionsPane", "reactions", "QCiRig.Dock.Reactions",
	 Qt::RightDockWidgetArea, true, false, GROUP_PANEL},
	{"qciRigChatDock", "QCiRigChatPane", "chat", "QCiRig.Dock.Chat",
	 Qt::RightDockWidgetArea, true, false, GROUP_PANEL},
	{"qciRigTankDock", "QCiRigTankPane", "emotes", "QCiRig.Dock.Tank",
	 Qt::RightDockWidgetArea, true, false, GROUP_PANEL},
	{"qciRigAudioDock", "QCiRigAudioPane", "audio", "QCiRig.Dock.Audio",
	 Qt::RightDockWidgetArea, true, false, GROUP_PANEL},
};
/* clang-format on */

/* Suggested size per dock, in the same order as RIG_DOCKS. Kept beside the table rather than in it
   so the table stays one screen wide and one grep away from being read as data. */
static const QSize RIG_DOCK_SIZES[] = {
	QSize(420, 560), QSize(420, 620), QSize(420, 520), QSize(420, 480), QSize(420, 360), QSize(420, 420),
};

static constexpr int RIG_DOCK_COUNT = int(sizeof(RIG_DOCKS) / sizeof(RIG_DOCKS[0]));

static_assert(sizeof(RIG_DOCK_SIZES) / sizeof(RIG_DOCK_SIZES[0]) == sizeof(RIG_DOCKS) / sizeof(RIG_DOCKS[0]),
	      "RIG_DOCK_SIZES must have one entry per RIG_DOCKS row");

/* ─────────────────────────────────────────────────────────────────────────────────────────────
 * THE SAFETY RULE, AS A MECHANISM RATHER THAN AS A PROMISE.
 *
 * A tabified QDockWidget that is not the current tab is not "small" or "collapsed" — it is gone.
 * There is no sliver of it on screen. So the rule is: a pane whose hidden state can cause the
 * operator to act wrongly (or to fail to act) is never given a tab bar to hide behind. PRIVACY
 * HOLD and the stream transport are the canonical case and the reason Tabifiable() exists — it is
 * defined constexpr in the header so it can be asked at BOTH times.
 *
 * It is asked at runtime ONCE, in ApplyDefaultLayout(), rather than trusting the `group` column to
 * have been typed correctly — a table is a promise and a predicate is a mechanism. A future row
 * that sets dangerousWhenHidden and a real group by mistake still cannot be hidden.
 *
 * ⚠️ AND IT IS ASKED AT COMPILE TIME, BELOW. This assertion is a TRANSFER, not a new idea:
 * BUILTIN_DOCKS in QCiBasic_Docks.cpp carried the application's only dangerousWhenHidden row
 * (controlsDock, because Go Live was on it) and asserted the property there. controlsDock is gone
 * and GO LIVE is on qciRigControlDock, so without this copy the deletion would have silently taken
 * a compile-time safety gate with it. The walk is over the whole table rather than naming
 * qciRigControlDock, so a SECOND dangerous pane added later is covered with nobody remembering to
 * extend an assertion — the same shape the builtin table used.
 */
constexpr bool no_dangerous_rig_dock_is_tabifiable()
{
	for (int i = 0; i < RIG_DOCK_COUNT; i++) {
		if (RIG_DOCKS[i].dangerousWhenHidden && Tabifiable(RIG_DOCKS[i])) {
			return false;
		}
	}
	return true;
}

static_assert(no_dangerous_rig_dock_is_tabifiable(),
	      "a rig dock marked dangerousWhenHidden was given a tab group — a tabified dock that is "
	      "not the current tab is INVISIBLE, and PRIVACY HOLD / GO LIVE must never be invisible");

/* ─────────────────────────────────────────────────────────────────────────────────────────────
 * BREAK A DOCK OUT OF A TAB GROUP AND PUT IT BACK ON THE GLASS.
 *
 * Tabifiable() is asked exactly once, while the default layout is being built, which makes it a
 * rule about how the rig SHIPS. The rule the header states is about how the rig RUNS: the main
 * window enables AllowTabbedDocks (frontend/forms/QCiBasic.ui) and the docks are unlocked unless
 * the operator locks them, so at any moment a drag can put this pane on somebody else's tab bar —
 * either by dropping it onto another dock, or by dropping another dock onto IT, which tabifies it
 * without it ever moving. Then PRIVACY HOLD is gone the moment a sibling tab is current, and
 * saveState() writes that layout out for every launch after this one.
 *
 * Returns true if it had to act, so the caller can log it: a pane that jumps back out of a tab bar
 * with nothing said anywhere reads as a bug rather than as a rule.
 */
static bool UntabifyDock(OBSBasic *main, QDockWidget *dock, Qt::DockWidgetArea homeArea)
{
	if (!main || !dock || dock->isFloating()) {
		/* A floating dock is its own window — on screen, filed behind nothing. The operator may
		   put this pane wherever they want it; they may not put it somewhere it can be
		   INVISIBLE, and those are different rules. */
		return false;
	}

	/* TWO SHAPES OF "BEHIND A TAB", which is why this is not a tabifiedDockWidgets() test alone.
	   A dock tabified inside the main window reports both its area and its tab mates. A dock
	   tabified inside a FLOATING group window (QMainWindow::GroupedDragging) reports neither: it
	   is not floating — the group window is — and it belongs to no dock area. That option is off
	   in QCiBasic.ui today, and an invariant about the panic control does not get to depend on a
	   dock option in a form file staying off. */
	const Qt::DockWidgetArea area = main->dockWidgetArea(dock);
	if (area != Qt::NoDockWidgetArea && main->tabifiedDockWidgets(dock).isEmpty()) {
		return false;
	}

	/* Back into the area it is in NOW, not the one the table ships it in. This is a repair, not a
	   layout reset: dragging the pane back across the window would cost the operator more than the
	   tab bar did. homeArea is the fallback for the group-window case, where there is no "now". */
	main->removeDockWidget(dock); /* ⚠️ removeDockWidget() also HIDES it — hence the setVisible() */
	main->addDockWidget(area != Qt::NoDockWidgetArea ? area : homeArea, dock);
	dock->setVisible(true);
	dock->raise();
	return true;
}

/* ─────────────────────────────────────────────────────────────────────────────────────────────
 * THE DOCK THAT CANNOT BE DISMISSED.
 *
 * ⚠️ QDockWidget::setFeatures() IS NOT VIRTUAL, so a subclass that "overrides" it is shadowing,
 * and every call through a QDockWidget* — which is every call OBSBasic makes — takes the base
 * version and puts the close button straight back. Three of those calls exist and they are not
 * ours: AddDockWidget() sets features on registration, on_lockDocks_toggled() rewrites them for
 * every extra dock whenever the lock changes, and a plugin may do it at any time.
 *
 * So the guard is hung on the SIGNAL instead. featuresChanged is emitted by whoever set them, so
 * this catches all three and anything added later, and the operator cannot arrive at a rig whose
 * panic control has a close button because a lock toggle happened to run last. It terminates:
 * setFeatures() inside the handler re-emits once with the bit already clear, and the guard is a
 * no-op the second time.
 *
 * closeEvent() is the other half — a dock with no close button can still be closed by a menu
 * action or by code, and this refuses that too.
 *
 * ⚠️ AND THE TABIFY RULE IS HUNG ON SIGNALS FOR THE SAME REASON. Tabifiable() runs when the layout
 * is built; a drag happens whenever the operator feels like it. See UntabifyDock() above and
 * ScheduleUntabify() below — hidden-behind-a-tab and hidden-by-a-close-button are the same failure
 * and this class refuses both at runtime rather than at construction time.
 */
class QCiRigPanelDock : public OBSDock {
public:
	QCiRigPanelDock(const QString &title, bool undismissable_, Qt::DockWidgetArea homeArea_)
		: OBSDock(title),
		  undismissable(undismissable_),
		  homeArea(homeArea_)
	{
		if (!undismissable) {
			return;
		}
		connect(this, &QDockWidget::featuresChanged, this, [this](QDockWidget::DockWidgetFeatures f) {
			if (f & QDockWidget::DockWidgetClosable) {
				setFeatures(f & ~QDockWidget::DockWidgetClosable);
			}
		});

		/* THE TABIFY GUARD NEEDS BOTH OF THESE, AND NEITHER ONE ALONE IS ENOUGH.
		 *
		 * dockLocationChanged fires when THIS dock is dropped somewhere — including into a tab
		 * group where it lands as the current tab. That is invisible-in-waiting rather than
		 * invisible, so nothing else in the program would ever notice it.
		 *
		 * visibilityChanged(false) catches the other direction: another dock dropped ONTO this
		 * one tabifies it without this dock moving at all, so there is no location change to
		 * hear — but Qt pushes a non-current tab off-screen and says so here. It is also what
		 * fires when the operator clicks a sibling tab, which is the moment PRIVACY HOLD
		 * actually leaves the glass. */
		connect(this, &QDockWidget::dockLocationChanged, this,
			[this](Qt::DockWidgetArea) { ScheduleUntabify(); });
		connect(this, &QDockWidget::visibilityChanged, this, [this](bool visible) {
			if (!visible) {
				ScheduleUntabify();
			}
		});
	}

	void closeEvent(QCloseEvent *event) override
	{
		if (undismissable && !OBSBasic::Get()->isClosing()) {
			/* Not "ask the operator whether they meant it" — refuse. The one control this
			   dock exists to keep on the glass is the one that hides their face. */
			event->ignore();
			return;
		}
		OBSDock::closeEvent(event);
	}

private:
	/* ⚠️ QUEUED, NOT INLINE, AND IT TERMINATES — AND IT WAITS FOR THE MOUSE.
	 *
	 * Both signals above are emitted from inside QMainWindowLayout while it is still applying a
	 * layout, so the repair cannot run there — it would re-enter the layout mid-move.
	 *
	 * ⚠️ BUT A ZERO-TIMER IS NOT "ONCE THIS DROP HAS SETTLED", WHICH IS THE OBVIOUS THING TO
	 * BELIEVE AND IS WRONG. visibilityChanged(false) is emitted from QDockAreaLayoutInfo::apply(),
	 * which QMainWindowLayout::hover() calls on every mouse-move WHILE THE DRAG IS STILL IN THE
	 * OPERATOR'S HAND — a drop has not happened and may never happen. A drag is driven by mouse
	 * events pumped through the same event loop, so a zero-timer queued mid-hover fires between two
	 * mouse-moves, i.e. still mid-drag, with QMainWindowLayout holding a live gap item for the
	 * floating dock. Calling removeDockWidget()/addDockWidget() at that moment mutates the layout
	 * out from under the drag that is still running.
	 *
	 * So the repair additionally refuses to act while any mouse button is down, and re-arms
	 * instead. Buttons-up is the honest "the operator has let go" signal; Qt exposes no public
	 * "is a dock drag in progress". The poll interval only bounds how soon after release the
	 * repair lands, and PRIVACY HOLD spends that window VISIBLE (it is mid-drag, under the cursor),
	 * so a slow repair is not an unsafe one.
	 *
	 * The repair emits both of those signals again (removeDockWidget/addDockWidget), which is the
	 * loop to be afraid of. Two things stop it, and the second is the one that actually holds:
	 * `untabifying` swallows the synchronous echo, and UntabifyDock() is a NO-OP on a dock that is
	 * not tabified — which is precisely the state it just left this dock in, so a late echo that
	 * arrives after the flag has cleared (the dock animator emits some of these) finds nothing to
	 * do. Same shape as the featuresChanged guard above: the second pass is a no-op by
	 * construction, not by a flag being right. */
	static constexpr int UNTABIFY_DRAG_POLL_MS = 50;

	void ScheduleUntabify(int delayMs = 0)
	{
		if (untabifying || untabifyQueued) {
			return;
		}
		untabifyQueued = true;
		QTimer::singleShot(delayMs, this, [this]() {
			untabifyQueued = false;

			OBSBasic *main = OBSBasic::Get();
			if (!main || main->isClosing()) {
				return;
			}

			/* Still dragging — do not touch the layout. Re-arm and ask again. */
			if (QGuiApplication::mouseButtons() != Qt::NoButton) {
				ScheduleUntabify(UNTABIFY_DRAG_POLL_MS);
				return;
			}

			untabifying = true;
			const bool acted = UntabifyDock(main, this, homeArea);
			untabifying = false;

			if (acted) {
				/* Say it. A pane that leaves a tab bar on its own is otherwise
				   indistinguishable from a glitch. */
				blog(LOG_INFO,
				     "[QCiRigDocks] dock '%s' was tabified — pulled it back out; the pane "
				     "carrying PRIVACY HOLD is never allowed behind a tab",
				     QT_TO_UTF8(objectName()));
			}
		});
	}

	const bool undismissable;
	const Qt::DockWidgetArea homeArea;
	bool untabifyQueued = false;
	bool untabifying = false;
};

/* ── the base URL, through one accessor ────────────────────────────────────────────────────── */

/* ⚠️ LOOPBACK IS THE TRUST BOUNDARY, NOT A CONVENIENT DEFAULT.
 *
 * qci-rig's lib/hub.test.mjs keeps moderation state, donor names, dollar amounts and chat lines out
 * of /hub because /hub is the route a LAN device with a dumped flash can reach. THIS fork reads the
 * rest, and writes verbs the panels are refused, for exactly one reason: it is on the same machine
 * as the server. Move the address off-box and that reason is gone — and what replaces it is worse
 * than "no rig", because the fork would be handing PRIVACY HOLD, mask mode and the stream transport
 * to whatever host now answers that name.
 *
 * (This paragraph used to end "…because a host that answers ANYTHING at all reads as success on the
 * write path — QCiRigClient::write only reports a failure when nothing answered — so a mistyped
 * PRIVACY HOLD prints CUT, MASKED while the operator's face is still live." That was true when it
 * was written and is no longer: write() now establishes success POSITIVELY, requiring a 2xx and an
 * explicit ok:true, and reports a LINK FAULT otherwise. The loopback rule does not rest on that bug
 * being present, which is why it stays either way — but the sentence is left here, corrected, rather
 * than deleted, because "the address is validated so the write path does not have to be" is exactly
 * the trade nobody should infer from silence.)
 *
 * "localhost" is accepted because RFC 6761 reserves it for the loopback interface and it is what an
 * operator types. Everything else has to be an address QHostAddress agrees is loopback: the whole
 * v4 loopback block, the v6 loopback address, and the v4-mapped-into-v6 spelling of the former —
 * a wider net than the two or three literal strings anyone remembers to compare against, and one
 * that keeps this file from spelling the address a second time. */
static bool IsLoopbackHost(const QString &host)
{
	if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0) {
		return true;
	}
	const QHostAddress addr(host);
	return !addr.isNull() && addr.isLoopback();
}

/* The one test that decides whether an address may drive the panic button. Everything a route is
   appended to goes through here — the dialog on the way in and BaseUrl() on the way out. */
static bool IsUsableRigUrl(const QUrl &url)
{
	if (!url.isValid() || url.host().isEmpty()) {
		return false;
	}
	if (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https")) {
		return false;
	}

	/* NO PATH, NO QUERY, NO FRAGMENT, NO USERINFO — this is a base, and routes are concatenated
	   onto it verbatim ("/hub/action?a=hold", built by WithQuery in QCiRigClient.cpp). A base of
	   ".../panel" makes every route a 404 and a base carrying "?x=1" makes every query malformed,
	   and per the note above a 404 from a live server is printed to the operator as success. The
	   dialog's own help text has always promised "and no path"; this is where it becomes true. */
	if (!url.path().isEmpty() && url.path() != QStringLiteral("/")) {
		return false;
	}
	if (url.hasQuery() || url.hasFragment() || !url.userInfo().isEmpty()) {
		return false;
	}

	return IsLoopbackHost(url.host());
}

QString DefaultBaseUrl()
{
	return QString::fromUtf8(QCI_RIG_BASE_URL_DEFAULT);
}

QString BaseUrl()
{
	const char *configured = config_get_string(App()->GetUserConfig(), CFG_SECTION, CFG_BASE_URL);
	QString url = (configured && *configured) ? QString::fromUtf8(configured) : DefaultBaseUrl();

	/* A trailing slash here and a leading slash in the route is how "//hub" gets served as a 404
	   that looks exactly like a panel that has not finished loading. */
	while (url.endsWith('/')) {
		url.chop(1);
	}

	/* ⚠️ THE DIALOG IS NOT THE ONLY WAY IN. user.ini is a text file, an older build wrote this key
	   with no loopback rule at all, and a synced or restored profile arrives with whatever it
	   arrives with. So the check lives HERE, at the one accessor every reader goes through, rather
	   than only at the keyboard — and it fails CLOSED: an address that may not drive PRIVACY HOLD
	   is replaced by the built-in loopback default, which either finds the rig or reports it
	   unreachable. Both of those are honest; talking to a stranger is not. */
	if (!IsUsableRigUrl(QUrl(url))) {
		/* One line per distinct bad value: this runs on every request. */
		static QString warned;
		if (warned != url) {
			warned = url;
			blog(LOG_WARNING,
			     "[QCiRigDocks] configured base URL '%s' is not a usable loopback address — "
			     "falling back to %s (the rig's events server is loopback-only, see "
			     "QCiRigClient.hpp)",
			     QT_TO_UTF8(url), QCI_RIG_BASE_URL_DEFAULT);
		}
		return DefaultBaseUrl();
	}

	return url;
}

bool SetBaseUrl(const QString &url)
{
	QString trimmed = url.trimmed();
	while (trimmed.endsWith('/')) {
		trimmed.chop(1);
	}

	config_set_string(App()->GetUserConfig(), CFG_SECTION, CFG_BASE_URL, QT_TO_UTF8(trimmed));

	/* ⚠️ THE RESULT IS RETURNED HERE EVEN THOUGH THE REST OF THIS FRONTEND DISCARDS IT, and the
	   difference is what is being written. config_set_string() cannot fail, so a failed save leaves
	   the new address live in memory for the whole session and the OLD one on disk: the operator
	   points the panel at the rig, watches it work, restarts, and is now driving a different
	   machine's privacy control with no event anywhere in between. Every other config_save_safe()
	   in the frontend is persisting a preference; this one is persisting WHICH MACHINE THE PANIC
	   BUTTON TALKS TO, so the caller is given the chance to say so. */
	return config_save_safe(App()->GetUserConfig(), "tmp", nullptr) == CONFIG_SUCCESS;
}

/* ── the seam ──────────────────────────────────────────────────────────────────────────────── */

/*
 * Build the body of one dock.
 *
 * THIS FUNCTION IS THE ENTIRE SURFACE A NEW PANE HAS TO TOUCH. Registration, the Docks menu, the
 * default layout, the tabify rule, the undismissable rule, the config key and the base URL are all
 * above and none of them care what a dock contains.
 *
 * The switch is on the table's `pane` string rather than on an enum so that the table stays plain
 * data that qci-rig/lib/studiodocks.test.mjs can read out of this file and check against the pane
 * source — the fork is the copy in another language with no way to notice when the two drift.
 */
QWidget *MakeRigDockContent(QDockWidget *dock, const RigDock &spec)
{
	const QString pane = QString::fromUtf8(spec.pane);
	QWidget *content = nullptr;

	if (pane == QLatin1String("QCiRigControlPane")) {
		content = new QCiRigControlPane(dock);
	} else if (pane == QLatin1String("QCiRigQueuePane")) {
		content = new QCiRigQueuePane(dock);
	} else if (pane == QLatin1String("QCiRigReactionsPane")) {
		content = new QCiRigReactionsPane(dock);
	} else if (pane == QLatin1String("QCiRigChatPane")) {
		content = new QCiRigChatPane(dock);
	} else if (pane == QLatin1String("QCiRigTankPane")) {
		content = new QCiRigTankPane(dock);
	} else if (pane == QLatin1String("QCiRigAudioPane")) {
		content = new QCiRigAudioPane(dock);
	}

	if (!content) {
		blog(LOG_WARNING, "[QCiRigDocks] no pane class named '%s' — dock '%s' not registered", spec.pane,
		     spec.objectName);
		return nullptr;
	}

	dock->setWidget(content);
	return content;
}

/* The dock shell. */
static QDockWidget *CreateRigDock(const RigDock &spec, const QSize &size)
{
	/* The area is handed over as well as used below: it is the dock's only fallback for "where do
	   I put myself back" when it is pulled out of a tab group that has no dock area of its own. */
	QCiRigPanelDock *dock = new QCiRigPanelDock(QTStr(spec.localeKey), spec.dangerousWhenHidden, spec.area);
	dock->setObjectName(QString::fromUtf8(spec.objectName));
	dock->setWindowTitle(QTStr(spec.localeKey));
	dock->setAllowedAreas(Qt::AllDockWidgetAreas);
	dock->setMinimumSize(240, 140);
	dock->resize(size);
	return dock;
}

/* ── registration ──────────────────────────────────────────────────────────────────────────── */

void Load(OBSBasic *main, QMenu *docksMenu)
{
	if (!main) {
		return;
	}

	config_set_default_string(App()->GetUserConfig(), CFG_SECTION, CFG_BASE_URL, QCI_RIG_BASE_URL_DEFAULT);

	for (int i = 0; i < RIG_DOCK_COUNT; i++) {
		const RigDock &spec = RIG_DOCKS[i];
		const QString name = QString::fromUtf8(spec.objectName);

		/* A plugin, or a profile carrying a dock from an older build, may already own this
		   name. Two docks with one objectName fuse under saveState/restoreState. */
		if (main->IsDockObjectNameUsed(name)) {
			blog(LOG_WARNING, "[QCiRigDocks] dock name '%s' is already in use — not registering",
			     spec.objectName);
			continue;
		}

		QDockWidget *dock = CreateRigDock(spec, RIG_DOCK_SIZES[i]);
		if (!MakeRigDockContent(dock, spec)) {
			delete dock;
			continue;
		}

		main->AddDockWidget(dock, spec.area);

		if (spec.dangerousWhenHidden && docksMenu) {
			/* ⚠️ AND THE MENU ITEM GOES TOO. AddDockWidget() adds every dock's
			   toggleViewAction() to Docks; leaving it there for this one would be a
			   one-click way to hide the panic control that the featuresChanged guard above
			   never sees, because a toggle action calls setVisible() and not close(). Both
			   doors, or neither. */
			docksMenu->removeAction(dock->toggleViewAction());
		}
	}

	if (docksMenu) {
		QAction *action = docksMenu->addAction(QTStr("Basic.MainMenu.Docks.QCiRigPanelURL"));
		QObject::connect(action, &QAction::triggered, main, [main]() { PromptForBaseUrl(main); });
	}

	/* THE ONE CLIENT STARTS ONCE, HERE, AFTER THE PANES EXIST. It is parented to the application
	   rather than to any dock, so a dock the operator closes cannot take the state feed with it. */
	QCiRigClient::Get()->start();
}

/* ── layout ────────────────────────────────────────────────────────────────────────────────── */

void ApplyDefaultLayout(OBSBasic *main)
{
	if (!main) {
		return;
	}

	QDockWidget *found[RIG_DOCK_COUNT] = {};
	int placed = 0;

	/* Pass 1 — home every dock in its area, in table order. Qt stacks same-area docks in the
	   order they are added, so CONTROL lands above the tab group in the right column without
	   anybody having to compute a splitter. This also undoes on_resetDocks_triggered()'s
	   RESET_DOCKLIST, which floats and hides every extra dock — including these. */
	for (int i = 0; i < RIG_DOCK_COUNT; i++) {
		found[i] = main->findChild<QDockWidget *>(QString::fromUtf8(RIG_DOCKS[i].objectName));
		if (!found[i]) {
			continue;
		}
		placed++;
		found[i]->setFloating(false);
		main->addDockWidget(RIG_DOCKS[i].area, found[i]);
	}

	if (!placed) {
		return;
	}

	/* Pass 2 — tabify, and ONLY where Tabifiable() allows it. See the rule in the header: a dock
	   that is not the current tab is invisible, so PRIVACY HOLD and the stream transport never
	   get one.

	   The leader of each group is held as a TABLE INDEX rather than as a pointer, because passes
	   4 and 5 need the spec's declared intent and not just the widget. */
	QHash<int, int> leader;
	for (int i = 0; i < RIG_DOCK_COUNT; i++) {
		if (!found[i] || !Tabifiable(RIG_DOCKS[i])) {
			continue;
		}
		const int group = RIG_DOCKS[i].group;
		if (leader.contains(group)) {
			main->tabifyDockWidget(found[leader.value(group)], found[i]);
		} else {
			leader.insert(group, i);
		}
	}

	/* Pass 3 — visibility. After the tabify, so a dock that is meant to be hidden does not leave
	   a live tab behind it. */
	for (int i = 0; i < RIG_DOCK_COUNT; i++) {
		if (found[i]) {
			found[i]->setVisible(RIG_DOCKS[i].visibleByDefault);
		}
	}

	/* Pass 4 — make the first member of each group the current tab. Without this Qt picks the
	   LAST dock added to the tab bar, so a fresh profile would open on Audio.

	   ⚠️ THE TEST IS visibleByDefault, NOT isVisible(). This function runs from OBSInit, and OBS
	   starts hidden when the system-tray options say so (opt_minimize_tray / SysTrayWhenStarted) —
	   QWidget::isVisible() is false for EVERY child of a window that has not been shown, so an
	   isVisible() guard here silently skips the raise and the resize on exactly the startup path
	   nobody tests by eye. The spec is the intent; the widget's current state is a side effect of
	   when this happened to run. */
	for (auto it = leader.constBegin(); it != leader.constEnd(); ++it) {
		const int idx = it.value();
		if (RIG_DOCKS[idx].visibleByDefault) {
			found[idx]->raise();
		}
	}

	/* Pass 5 — a column wide enough to read a queue in, capped so it cannot eat the preview. */
	QList<QDockWidget *> rightColumn;
	for (int i = 0; i < RIG_DOCK_COUNT; i++) {
		if (found[i] && RIG_DOCKS[i].area == Qt::RightDockWidgetArea && RIG_DOCKS[i].visibleByDefault) {
			rightColumn.append(found[i]);
		}
	}
	if (!rightColumn.isEmpty()) {
		const int width = std::min(main->width() * 30 / 100, 440);
		QList<int> widths;
		for (int i = 0; i < rightColumn.size(); i++) {
			widths.append(width);
		}
		main->resizeDocks(rightColumn, widths, Qt::Horizontal);
	}

	config_set_bool(App()->GetUserConfig(), CFG_SECTION, CFG_DOCKS_PLACED, true);
	config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
}

void ApplyDefaultLayoutIfNeeded(OBSBasic *main)
{
	/* Qt's restoreState() has nothing to say about a dock that did not exist when the state was
	   written: it leaves it wherever addDockWidget() put it, untabified and unsized. That is the
	   operator's OWN profile on the first run after this ships — six stacked strips in the right
	   column — so the default layout is applied once, keyed off the profile, and never again. */
	if (config_get_bool(App()->GetUserConfig(), CFG_SECTION, CFG_DOCKS_PLACED)) {
		return;
	}
	ApplyDefaultLayout(main);
}

void EnsureUndismissableVisible(OBSBasic *main)
{
	if (!main) {
		return;
	}
	for (int i = 0; i < RIG_DOCK_COUNT; i++) {
		if (!RIG_DOCKS[i].dangerousWhenHidden) {
			continue;
		}
		QDockWidget *dock = main->findChild<QDockWidget *>(QString::fromUtf8(RIG_DOCKS[i].objectName));
		if (!dock) {
			continue;
		}
		/* ⚠️ UNTABIFY FIRST, AND IT IS NOT BELT AND BRACES. This function runs after
		   restoreState(), and a saved DockState is a file the previous session wrote: if the
		   operator ever dragged this dock onto a tab bar, that is the layout being restored right
		   now. The runtime guard in QCiRigPanelDock would repair it a moment later, but only for a
		   dock this file actually built — registration is skipped when a plugin already owns the
		   objectName, and the findChild() above will happily hand back that foreign dock. The
		   panic control's visibility does not get to depend on who built the widget. */
		UntabifyDock(main, dock, RIG_DOCKS[i].area);

		/* setVisible() ALONE IS NOT ENOUGH FOR A TABIFIED DOCK — it is a no-op when the dock is
		   in a tab group and is not the current tab, which is the bug the fork already fixed in
		   setupDockAction(). The line above has just made sure this dock is in no tab group at
		   all; raise() stays because the two together are what put it in front, and because a
		   future row must not be able to inherit the no-op by being moved into a group. */
		dock->setVisible(true);
		dock->raise();
	}
}

/* ── the setting's UI ──────────────────────────────────────────────────────────────────────── */

void PromptForBaseUrl(OBSBasic *main)
{
	bool accepted = false;
	const QString entered = QInputDialog::getText(main, QTStr("QCiRig.BaseUrl.Title"),
						      QTStr("QCiRig.BaseUrl.Prompt"), QLineEdit::Normal, BaseUrl(),
						      &accepted);
	if (!accepted) {
		return;
	}

	const QString candidate = entered.trimmed();

	/* ⚠️ ONE PREDICATE, AND IT INCLUDES THE LOOPBACK RULE. A scheme-and-host check is the shape of
	   validation that looks thorough and lets the operator retarget the panic button at any machine
	   on the internet with one typo — see IsUsableRigUrl() for why a wrong host that ANSWERS is
	   worse than one that does not exist. */
	if (!IsUsableRigUrl(QUrl(candidate))) {
		/* The example in that string is %1 and is filled in from the constant above — an
		   address spelled out in help text is a copy that never throws when it goes stale. It is
		   also the answer here: the built-in default IS the loopback shape being asked for, so
		   the one warning this dialog owns still points at the right thing. The log line is where
		   the specific reason lives, because there is no locale key for each of them. */
		blog(LOG_WARNING,
		     "[QCiRigDocks] refused base URL '%s' — it must be http/https, on loopback, "
		     "with no path, query or fragment",
		     QT_TO_UTF8(candidate));
		OBSMessageBox::warning(main, QTStr("QCiRig.BaseUrl.Title"),
				       QTStr("QCiRig.BaseUrl.Invalid").arg(DefaultBaseUrl()));
		return;
	}

	if (!SetBaseUrl(candidate)) {
		/* THE SETTING IS LIVE BUT NOT SAVED, and the operator has to hear it now rather than
		   discover it after a restart. Untranslated on purpose: there is no locale key for this
		   case yet (add QCiRig.BaseUrl.SaveFailed next time en-US.ini is open), and English text
		   the operator can act on beats a silent divergence between what the panel is doing and
		   what the file says it will do next launch. The panel is left pointed at the new address
		   either way — refusing to use it would be a second surprise on top of the first. */
		blog(LOG_WARNING, "[QCiRigDocks] base URL set to '%s' for this session but could NOT be saved",
		     QT_TO_UTF8(candidate));
		OBSMessageBox::warning(main, QTStr("QCiRig.BaseUrl.Title"),
				       QStringLiteral("The panel is on %1 for this session, but the setting "
						      "could not be written to disk. It will be back on the "
						      "previous address after the next restart.")
					       .arg(candidate));
	}

	/* Nothing to re-point: every pane reads the model, and the one client reads BaseUrl() on each
	   request. One refresh and the whole panel is on the new address. */
	QCiRigClient::Get()->refresh();
}

} // namespace QCiRigDocks
