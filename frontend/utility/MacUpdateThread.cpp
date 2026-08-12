#include "MacUpdateThread.hpp"

#include <OBSApp.hpp>
#include <utility/WhatsNewInfoThread.hpp>

#include <qt-wrappers.hpp>
#include <ui-config.h>

#include "moc_MacUpdateThread.cpp"

/* A fork must not query the parent project's update service. The branch list it returns describes
 * *OBS Studio's* release channels, and every request carries the install GUID header from
 * WhatsNewInfoThread.cpp — QCi-Studio would be counted as, and asking to be updated to, upstream OBS.
 * Empty disables the fetch below; this whole file is also uncompiled while Sparkle is off (see
 * frontend/cmake/feature-sparkle.cmake), so this is the second lock on the same door. */
static const char *MAC_BRANCHES_URL = "";
static const char *MAC_DEFAULT_BRANCH = "stable";

bool GetBranch(std::string &selectedBranch)
{
	const char *config_branch = config_get_string(App()->GetAppConfig(), "General", "UpdateBranch");
	if (!config_branch) {
		return true;
	}

	bool found = false;
	for (const UpdateBranch &branch : App()->GetBranches()) {
		if (branch.name != config_branch) {
			continue;
		}
		/* A branch that is found but disabled will just silently fall back to
		 * the default. But if the branch was removed entirely, the user should
		 * be warned, so leave this false *only* if the branch was removed. */
		found = true;

		if (branch.is_enabled) {
			selectedBranch = branch.name.toStdString();
		}
		break;
	}

	return found;
}

void MacUpdateThread::infoMsg(const QString &title, const QString &text)
{
	OBSMessageBox::information(App()->GetMainWindow(), title, text);
}

void MacUpdateThread::info(const QString &title, const QString &text)
{
	QMetaObject::invokeMethod(this, "infoMsg", Qt::BlockingQueuedConnection, Q_ARG(QString, title),
				  Q_ARG(QString, text));
}

void MacUpdateThread::run()
try {
	std::string text;
	std::string branch = MAC_DEFAULT_BRANCH;

	/* ----------------------------------- *
	 * get branches from server            */

	if (MAC_BRANCHES_URL[0] != '\0' &&
	    FetchAndVerifyFile("branches", OBS_USER_DATA_DIR "/updates/branches.json", MAC_BRANCHES_URL, &text)) {
		App()->SetBranchData(text);
	}

	/* ----------------------------------- *
	 * Validate branch selection           */

	if (!GetBranch(branch)) {
		config_set_string(App()->GetAppConfig(), "General", "UpdateBranch", MAC_DEFAULT_BRANCH);
		info(QTStr("Updater.BranchNotFound.Title"), QTStr("Updater.BranchNotFound.Text"));
	}

	emit Result(QString::fromStdString(branch), manualUpdate);

} catch (std::string &text) {
	blog(LOG_WARNING, "%s: %s", __FUNCTION__, text.c_str());
}
