#include "QCiAbout.hpp"

#include <widgets/QCiBasic.hpp>
#include <utility/RemoteTextThread.hpp>

#include <qt-wrappers.hpp>

#include <json11.hpp>

#include "moc_QCiAbout.cpp"

using namespace json11;

extern bool steam;

OBSAbout::OBSAbout(QWidget *parent) : QDialog(parent), ui(new Ui::OBSAbout)
{
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

	ui->setupUi(this);

	QString bitness;

	if (sizeof(void *) == 4) {
		bitness = " (32 bit)";
	} else if (sizeof(void *) == 8) {
		bitness = " (64 bit)";
	}

	QString ver = obs_get_version_string();

	ui->version->setText(ver + bitness);

	ui->contribute->setText(QTStr("About.Contribute"));

	/* The donate and get-involved links pointed at obsproject.com. A fork that solicits money
	 * for the project it forked, from its own About box, is asking the operator to fund somebody
	 * else's roadmap under this application's name. Both rows are gone, not relinked. */
	delete ui->donate;
	delete ui->getInvolved;

	ui->about->setText("<a href='#'>" + QTStr("About") + "</a>");
	ui->authors->setText("<a href='#'>" + QTStr("About.Authors") + "</a>");
	ui->license->setText("<a href='#'>" + QTStr("About.License") + "</a>");

	ui->name->setProperty("class", "text-heading");
	ui->version->setProperty("class", "text-large");
	ui->about->setProperty("class", "bg-base");
	ui->authors->setProperty("class", "bg-base");
	ui->license->setProperty("class", "bg-base");
	ui->info->setProperty("class", "");

	connect(ui->about, &ClickableLabel::clicked, this, &OBSAbout::ShowAbout);
	connect(ui->authors, &ClickableLabel::clicked, this, &OBSAbout::ShowAuthors);
	connect(ui->license, &ClickableLabel::clicked, this, &OBSAbout::ShowLicense);

	/* Opening About used to fire an HTTPS GET at obsproject.com for upstream's Patreon roll and
	 * render it as this application's credits. It was the only network request the dialog made,
	 * it identified the machine to upstream every time the box was opened, and what it drew was a
	 * list of people who fund a different project. */
	ShowAbout();
}

void OBSAbout::ShowAbout()
{
	ui->textBrowser->setHtml(QTStr("About.Text"));
}

void OBSAbout::ShowAuthors()
{
	std::string path;
	QString error = QTStr("About.Error").arg("https://github.com/obsproject/obs-studio/blob/master/AUTHORS");

#ifdef __APPLE__
	if (!GetDataFilePath("AUTHORS", path)) {
#else
	if (!GetDataFilePath("authors/AUTHORS", path)) {
#endif
		ui->textBrowser->setPlainText(error);
		return;
	}

	ui->textBrowser->setPlainText(QString::fromStdString(path));

	BPtr<char> text = os_quick_read_utf8_file(path.c_str());

	if (!text || !*text) {
		ui->textBrowser->setPlainText(error);
		return;
	}

	ui->textBrowser->setPlainText(QT_UTF8(text));
}

void OBSAbout::ShowLicense()
{
	std::string path;
	QString error = QTStr("About.Error").arg("https://github.com/obsproject/obs-studio/blob/master/COPYING");

	if (!GetDataFilePath("license/gplv2.txt", path)) {
		ui->textBrowser->setPlainText(error);
		return;
	}

	BPtr<char> text = os_quick_read_utf8_file(path.c_str());

	if (!text || !*text) {
		ui->textBrowser->setPlainText(error);
		return;
	}

	ui->textBrowser->setPlainText(QT_UTF8(text));
}
