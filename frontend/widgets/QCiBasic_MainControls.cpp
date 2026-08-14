/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>
                          Zachary Lund <admin@computerquip.com>
                          Philippe Groarke <philippe.groarke@gmail.com>

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

#include "QCiBasic.hpp"
#include "QCiBasicStats.hpp"

#include <dialogs/LogUploadDialog.hpp>
#include <dialogs/QCiAbout.hpp>
#include <dialogs/QCiBasicAdvAudio.hpp>
#include <dialogs/QCiBasicFilters.hpp>
#include <dialogs/QCiBasicInteraction.hpp>
#include <dialogs/QCiBasicProperties.hpp>
#include <dialogs/QCiBasicTransform.hpp>
#ifdef ENABLE_IDIAN_PLAYGROUND
#include <dialogs/QCiIdianPlayground.hpp>
#endif
#include <dialogs/QCiLogViewer.hpp>
#ifdef __APPLE__
#include <dialogs/QCiPermissions.hpp>
#endif
#include <dialogs/QCiRemux.hpp>
#include <settings/QCiBasicSettings.hpp>
#include <utility/RemoteTextThread.hpp>

#include <qt-wrappers.hpp>
#include <ui-config.h>

#include <nlohmann/json.hpp>
#include <QDesktopServices>

#ifdef _WIN32
#include <sstream>
#endif

extern bool restart;
extern bool restart_safe;
extern volatile long insideEventLoop;
extern bool safe_mode;

struct QCef;
struct QCefCookieManager;

extern QCef *cef;
extern QCefCookieManager *panel_cookies;

using namespace std;
using LogUploadDialog = OBS::LogUploadDialog;
using LogUploadType = OBS::LogFileType;

void OBSBasic::CreateInteractionWindow(obs_source_t *source)
{
	bool closed = true;
	if (interaction) {
		closed = interaction->close();
	}

	if (!closed) {
		return;
	}

	interaction = new OBSBasicInteraction(this, source);
	interaction->Init();
	interaction->setAttribute(Qt::WA_DeleteOnClose, true);
}

void OBSBasic::CreatePropertiesWindow(obs_source_t *source)
{
	bool closed = true;
	if (properties) {
		closed = properties->close();
	}

	if (!closed) {
		return;
	}

	properties = new OBSBasicProperties(this, source);
	properties->Init();
	properties->setAttribute(Qt::WA_DeleteOnClose, true);
}

void OBSBasic::CreateFiltersWindow(obs_source_t *source)
{
	bool closed = true;
	if (filters) {
		closed = filters->close();
	}

	if (!closed) {
		return;
	}

	filters = new OBSBasicFilters(this, source);
	filters->Init();
	filters->setAttribute(Qt::WA_DeleteOnClose, true);
}

void OBSBasic::ResetUI()
{
	bool studioPortraitLayout = config_get_bool(App()->GetUserConfig(), "BasicWindow", "StudioPortraitLayout");

	if (studioPortraitLayout) {
		ui->previewLayout->setDirection(QBoxLayout::BottomToTop);
	} else {
		ui->previewLayout->setDirection(QBoxLayout::LeftToRight);
	}

	UpdatePreviewProgramIndicators();
}

void OBSBasic::CloseDialogs()
{
	QList<QDialog *> childDialogs = this->findChildren<QDialog *>();
	if (!childDialogs.isEmpty()) {
		for (int i = 0; i < childDialogs.size(); ++i) {
			childDialogs.at(i)->close();
		}
	}

	/* No `stats` window to close any more — Stats is the dock, and a dock's geometry is saved by
	 * saveState() into DockState rather than by a widget close handler. See on_stats_triggered(). */
	if (!remux.isNull()) {
		remux->close();
	}
}

void OBSBasic::EnumDialogs()
{
	visDialogs.clear();
	modalDialogs.clear();
	visMsgBoxes.clear();

	/* fill list of Visible dialogs and Modal dialogs */
	QList<QDialog *> dialogs = findChildren<QDialog *>();
	for (QDialog *dialog : dialogs) {
		if (dialog->isVisible()) {
			visDialogs.append(dialog);
		}
		if (dialog->isModal()) {
			modalDialogs.append(dialog);
		}
	}

	/* fill list of Visible message boxes */
	QList<QMessageBox *> msgBoxes = findChildren<QMessageBox *>();
	for (QMessageBox *msgbox : msgBoxes) {
		if (msgbox->isVisible()) {
			visMsgBoxes.append(msgbox);
		}
	}
}

void OBSBasic::on_actionRemux_triggered()
{
	if (!remux.isNull()) {
		remux->show();
		remux->raise();
		return;
	}

	const char *mode = config_get_string(activeConfiguration, "Output", "Mode");
	const char *path = strcmp(mode, "Advanced") ? config_get_string(activeConfiguration, "SimpleOutput", "FilePath")
						    : config_get_string(activeConfiguration, "AdvOut", "RecFilePath");

	OBSRemux *remuxDlg;
	remuxDlg = new OBSRemux(path, this);
	remuxDlg->show();
	remux = remuxDlg;
}

void OBSBasic::on_action_Settings_triggered()
{
	static bool settings_already_executing = false;

	/* Do not load settings window if inside of a temporary event loop
	 * because we could be inside of an Auth::LoadUI call.  Keep trying
	 * once per second until we've exit any known sub-loops. */
	if (os_atomic_load_long(&insideEventLoop) != 0) {
		QTimer::singleShot(1000, this, &OBSBasic::on_action_Settings_triggered);
		return;
	}

	if (settings_already_executing) {
		return;
	}

	settings_already_executing = true;

	{
		OBSBasicSettings settings(this);
		settings.exec();
	}

	settings_already_executing = false;

	if (restart) {
		QMessageBox::StandardButton button =
			OBSMessageBox::question(this, QTStr("Restart"), QTStr("NeedsRestart"));

		if (button == QMessageBox::Yes) {
			close();
		} else {
			restart = false;
		}
	}
}

void OBSBasic::on_actionShowMacPermissions_triggered()
{
#ifdef __APPLE__
	OBSPermissions check(this, CheckPermission(kScreenCapture), CheckPermission(kVideoDeviceAccess),
			     CheckPermission(kAudioDeviceAccess), CheckPermission(kInputMonitoring));
	check.exec();
#endif
}

void OBSBasic::on_actionAdvAudioProperties_triggered()
{
	if (advAudioWindow != nullptr) {
		advAudioWindow->raise();
		return;
	}

	bool iconsVisible = config_get_bool(App()->GetUserConfig(), "BasicWindow", "ShowSourceIcons");

	advAudioWindow = new OBSBasicAdvAudio(this);
	advAudioWindow->show();
	advAudioWindow->setAttribute(Qt::WA_DeleteOnClose, true);
	advAudioWindow->SetIconsVisible(iconsVisible);
}

static BPtr<char> ReadLogFile(const char *subdir, const char *log)
{
	char logDir[512];
	if (GetAppConfigPath(logDir, sizeof(logDir), subdir) <= 0) {
		return nullptr;
	}

	string path = logDir;
	path += "/";
	path += log;

	BPtr<char> file = os_quick_read_utf8_file(path.c_str());
	if (!file) {
		blog(LOG_WARNING, "Failed to read log file %s", path.c_str());
	}

	return file;
}

/* A FORK MUST NOT POST ITS LOGS TO THE PARENT PROJECT. Empty means the feature is off, exactly as
 * crashUploadURL does in frontend/utility/CrashHandler.cpp — one shape for "this build does not
 * phone home", so the next person looking for it finds both.
 *
 * This was missed when the crash uploader was disabled, because it is a SECOND, unrelated upload
 * path: Help -> Upload Current Log File, with the URL hardcoded inline at the call site rather than
 * read from a constant. An OBS log names your scenes, sources, capture devices, monitors and file
 * paths, so this is a privacy leak with a menu item attached, not merely a wrong hostname. */
constexpr std::string_view logUploadURL = "";

void OBSBasic::UploadLog(const char *subdir, const char *file, const LogUploadType uploadType)
{
	if constexpr (logUploadURL.empty()) {
		blog(LOG_INFO, "Log upload is disabled in this build; the log stays on this machine");
		emit App()->logUploadFailed(uploadType, QTStr("LogUploadDialog.Errors.NoLogFile"));
		return;
	}

	BPtr<char> fileString{ReadLogFile(subdir, file)};

	if (!fileString || !*fileString) {
		OBSApp *app = App();
		emit app->logUploadFailed(uploadType, QTStr("LogUploadDialog.Errors.NoLogFile"));
		return;
	}

	ui->menuLogFiles->setEnabled(false);

	stringstream ss;
	ss << OBS_PRODUCT_NAME " " << App()->GetVersionString(false) << " log file uploaded at "
	   << CurrentDateTimeString()
	   << ((uploadType == OBS::LogFileType::CurrentAppLog) ? " (Active Log)" : " (Complete Log)") << "\n\n"
	   << fileString;

	if (logUploadThread) {
		logUploadThread->wait();
	}

	RemoteTextThread *thread = new RemoteTextThread(std::string(logUploadURL), "text/plain", ss.str());

	logUploadThread.reset(thread);

	connect(thread, &RemoteTextThread::Result, this,
		[this, uploadType](const std::string &text, const std::string &error) {
			logUploadFinished(text, error, uploadType);
		});

	logUploadThread->start();
}

void OBSBasic::on_actionShowLogs_triggered()
{
	char logDir[512];
	if (GetAppConfigPath(logDir, sizeof(logDir), OBS_USER_DATA_DIR "/logs") <= 0) {
		return;
	}

	QUrl url = QUrl::fromLocalFile(QT_UTF8(logDir));
	QDesktopServices::openUrl(url);
}

void OBSBasic::on_actionViewCurrentLog_triggered()
{
	if (!logView) {
		logView = new OBSLogViewer();
	}

	logView->show();
	logView->setWindowState((logView->windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
	logView->activateWindow();
	logView->raise();
}

void OBSBasic::on_actionShowCrashLogs_triggered()
{
	App()->openCrashLogDirectory();
}

void OBSBasic::on_actionRestartSafe_triggered()
{
	QMessageBox::StandardButton button = OBSMessageBox::question(
		this, QTStr("Restart"), safe_mode ? QTStr("SafeMode.RestartNormal") : QTStr("SafeMode.Restart"));

	if (button == QMessageBox::Yes) {
		restart = safe_mode;
		restart_safe = !safe_mode;
		close();
	}
}

void OBSBasic::logUploadFinished(const std::string &text, const std::string &error, LogUploadType uploadType)
{
	OBSApp *app = App();

	if (text.empty()) {
		emit app->logUploadFailed(uploadType, QString::fromStdString(error));
	} else {
		nlohmann::json parsed = nlohmann::json::parse(text);
		std::string logURL = parsed["url"];

		emit app->logUploadFinished(uploadType, QString::fromStdString(logURL));
	}
}

void OBSBasic::on_actionShowSettingsFolder_triggered()
{
	const std::string userConfigPath = App()->userConfigLocation.u8string() + "/" OBS_USER_DATA_DIR;
	const QString userConfigLocation = QString::fromStdString(userConfigPath);

	QDesktopServices::openUrl(QUrl::fromLocalFile(userConfigLocation));
}

void OBSBasic::on_actionShowProfileFolder_triggered()
{
	try {
		const OBSProfile &currentProfile = GetCurrentProfile();
		QString currentProfileLocation = QString::fromStdString(currentProfile.path.u8string());

		QDesktopServices::openUrl(QUrl::fromLocalFile(currentProfileLocation));
	} catch (const std::invalid_argument &error) {
		blog(LOG_ERROR, "%s", error.what());
	}
}

void OBSBasic::on_actionAlwaysOnTop_triggered()
{
#ifndef _WIN32
	/* Make sure all dialogs are safely and successfully closed before
	 * switching the always on top mode due to the fact that windows all
	 * have to be recreated, so queue the actual toggle to happen after
	 * all events related to closing the dialogs have finished */
	CloseDialogs();
#endif

	QMetaObject::invokeMethod(this, &OBSBasic::ToggleAlwaysOnTop, Qt::QueuedConnection);
}

void OBSBasic::ToggleAlwaysOnTop()
{
	bool isAlwaysOnTop = IsAlwaysOnTop(this);

	ui->actionAlwaysOnTop->setChecked(!isAlwaysOnTop);
	SetAlwaysOnTop(this, !isAlwaysOnTop);

	show();
}

void OBSBasic::CreateEditTransformWindow(obs_sceneitem_t *item)
{
	if (transformWindow) {
		transformWindow->close();
	}
	transformWindow = new OBSBasicTransform(item, this);
	connect(ui->scenes, &QListWidget::currentItemChanged, transformWindow, &OBSBasicTransform::onSceneChanged);
	transformWindow->show();
	transformWindow->setAttribute(Qt::WA_DeleteOnClose, true);
}

void OBSBasic::on_actionFullscreenInterface_triggered()
{
	if (!isFullScreen()) {
		showFullScreen();
	} else {
		showNormal();
	}
}

void OBSBasic::on_resetUI_triggered()
{
	on_resetDocks_triggered();
	setPreviewScalingWindow();

	ui->toggleListboxToolbars->setChecked(true);
	ui->toggleContextBar->setChecked(true);
	ui->toggleSourceIcons->setChecked(true);
	ui->toggleStatusBar->setChecked(true);
	ui->scenes->SetGridMode(false);
	ui->actionSceneListMode->setChecked(true);

	config_set_bool(App()->GetUserConfig(), "BasicWindow", "gridMode", false);
}

void OBSBasic::on_toggleListboxToolbars_toggled(bool visible)
{
	ui->sourcesToolbar->setVisible(visible);
	ui->scenesToolbar->setVisible(visible);

	config_set_bool(App()->GetUserConfig(), "BasicWindow", "ShowListboxToolbars", visible);
	emit userSettingChanged("BasicWindow", "ShowListboxToolbars");
}

void OBSBasic::on_toggleStatusBar_toggled(bool visible)
{
	ui->statusbar->setVisible(visible);

	config_set_bool(App()->GetUserConfig(), "BasicWindow", "ShowStatusBar", visible);
}

void OBSBasic::SetShowing(bool showing)
{
	if (!showing && isVisible()) {
		config_set_string(App()->GetUserConfig(), "BasicWindow", "geometry",
				  saveGeometry().toBase64().constData());

		/* hide all visible child dialogs */
		visDlgPositions.clear();
		if (!visDialogs.isEmpty()) {
			for (QDialog *dlg : visDialogs) {
				visDlgPositions.append(dlg->pos());
				dlg->hide();
			}
		}

		if (showHide) {
			showHide->setText(QTStr("Basic.SystemTray.Show"));
		}
		QTimer::singleShot(0, this, &OBSBasic::hide);

		if (previewEnabled) {
			EnablePreviewDisplay(false);
		}

#ifdef __APPLE__
		EnableOSXDockIcon(false);
#endif

	} else if (showing && !isVisible()) {
		if (showHide) {
			showHide->setText(QTStr("Basic.SystemTray.Hide"));
		}
		QTimer::singleShot(0, this, &OBSBasic::show);

		if (previewEnabled) {
			EnablePreviewDisplay(true);
		}

#ifdef __APPLE__
		EnableOSXDockIcon(true);
#endif

		/* raise and activate window to ensure it is on top */
		raise();
		activateWindow();

		/* show all child dialogs that was visible earlier */
		if (!visDialogs.isEmpty()) {
			for (int i = 0; i < visDialogs.size(); ++i) {
				QDialog *dlg = visDialogs[i];
				dlg->move(visDlgPositions[i]);
				dlg->show();
			}
		}

		/* Unminimize window if it was hidden to tray instead of task
		 * bar. */
		if (sysTrayMinimizeToTray()) {
			Qt::WindowStates state;
			state = windowState() & ~Qt::WindowMinimized;
			state |= Qt::WindowActive;
			setWindowState(state);
		}
	}
}

void OBSBasic::ToggleShowHide()
{
	bool showing = isVisible();
	if (showing) {
		/* check for modal dialogs */
		EnumDialogs();
		if (!modalDialogs.isEmpty() || !visMsgBoxes.isEmpty()) {
			return;
		}
	}
	SetShowing(!showing);
}

void OBSBasic::on_actionMainUndo_triggered()
{
	undo_s.undo();
}

void OBSBasic::on_actionMainRedo_triggered()
{
	undo_s.redo();
}

void OBSBasic::on_stats_triggered()
{
	/* ── ONE STATS, NOT TWO ──────────────────────────────────────────────────────────────────
	 *
	 * This used to `new OBSBasicStats(nullptr)` — a second, parentless, top-level Stats window,
	 * while statsDock has held a fully working OBSBasicStats since OBSInit. Two instances is not
	 * a cosmetic duplicate: each owns its own 2-second QTimer (QCiBasicStats.hpp) and its own
	 * os_cpu_usage_info_start() handle, and each registers its own obs_frontend_add_event_callback,
	 * so with both on screen the rig paid for two identical pollers measuring the same encoder.
	 *
	 * It also diverged in a way that made the duplicate the WRONG one to keep: the Reset-Stats
	 * hotkey walks findChildren<OBSBasicStats *>() (QCiBasic_Hotkeys.cpp), which reaches the dock's
	 * instance and never reached the parentless window — so "reset stats" silently did nothing to
	 * the window the operator was actually looking at.
	 *
	 * raise() matters and is not decoration: statsDock shares a tab bar with the mixer and
	 * transitions, and setVisible(true) on a tabified dock that is not the current tab is a no-op
	 * — the same trap setupDockAction() documents at length in QCiBasic_Docks.cpp. */
	if (statsDock) {
		statsDock->setVisible(true);
		statsDock->raise();
	}
}

void OBSBasic::on_idianPlayground_triggered()
{
#ifdef ENABLE_IDIAN_PLAYGROUND
	OBSIdianPlayground playground(this);
	playground.setModal(true);
	playground.show();
	playground.exec();
#endif
}

void OBSBasic::on_actionShowAbout_triggered()
{
	if (about) {
		about->close();
	}

	about = new OBSAbout(this);
	about->show();

	about->setAttribute(Qt::WA_DeleteOnClose, true);
}

void OBSBasic::on_OBSBasic_customContextMenuRequested(const QPoint &pos)
{
	QWidget *widget = childAt(pos);
	const char *className = nullptr;
	QString objName;
	if (widget != nullptr) {
		className = widget->metaObject()->className();
		objName = widget->objectName();
	}

	QPoint globalPos = mapToGlobal(pos);
	if (className && strstr(className, "Dock") != nullptr && !objName.isEmpty()) {
		if (objName.compare("scenesDock") == 0) {
			ui->scenes->customContextMenuRequested(globalPos);
		} else if (objName.compare("sourcesDock") == 0) {
			ui->sources->customContextMenuRequested(globalPos);
		}
	} else if (!className) {
		ui->menuDocks->exec(globalPos);
	}
}
