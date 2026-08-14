#include "QCiProjector.hpp"

#include <QCiApp.hpp>
#include <utility/display-helpers.hpp>
#include <utility/platform.hpp>
#include <widgets/QCiBasic.hpp>

#include <qt-wrappers.hpp>

#include <QScreen>
#include <QWindow>

#include "moc_QCiProjector.cpp"


OBSProjector::OBSProjector(QWidget *widget, obs_source_t *source_, int monitor, ProjectorType type_)
	: OBSQTDisplay(widget, Qt::Window),
	  weakSource(OBSGetWeakRef(source_))
{
	OBSSource source = GetSource();
	if (source) {
		sigs.emplace_back(obs_source_get_signal_handler(source), "rename", OBSSourceRenamed, this);
		sigs.emplace_back(obs_source_get_signal_handler(source), "destroy", OBSSourceDestroyed, this);
	}

	isAlwaysOnTop = config_get_bool(App()->GetUserConfig(), "BasicWindow", "ProjectorAlwaysOnTop");

	if (isAlwaysOnTop) {
		setWindowFlags(Qt::WindowStaysOnTopHint);
	}

	// Mark the window as a projector so SetDisplayAffinity
	// can skip it
	windowHandle()->setProperty("isOBSProjectorWindow", true);

#if defined(__linux__) || defined(__FreeBSD__) || defined(__DragonFly__)
	// Prevents resizing of projector windows
	setAttribute(Qt::WA_PaintOnScreen, false);
#endif

	type = type_;
#ifndef __APPLE__
	setWindowIcon(QIcon::fromTheme("obs", QIcon(":/res/images/qcis.png")));
#endif

	if (monitor == -1) {
		resize(480, 270);
	} else {
		SetMonitor(monitor);
	}

	if (source) {
		UpdateProjectorTitle(QT_UTF8(obs_source_get_name(source)));
	} else {
		UpdateProjectorTitle(QString());
	}

	QAction *action = new QAction(this);
	action->setShortcut(Qt::Key_Escape);
	addAction(action);
	connect(action, &QAction::triggered, this, &OBSProjector::EscapeTriggered);

	setAttribute(Qt::WA_DeleteOnClose, true);

	//disable application quit when last window closed
	setAttribute(Qt::WA_QuitOnClose, false);

	installEventFilter(CreateShortcutFilter());

	auto addDrawCallback = [this]() {
		obs_display_add_draw_callback(GetDisplay(), OBSRender, this);
		obs_display_set_background_color(GetDisplay(), 0x000000);
	};

	connect(this, &OBSQTDisplay::DisplayCreated, this, addDrawCallback);
	connect(App(), &QGuiApplication::screenRemoved, this, &OBSProjector::ScreenRemoved);

	App()->IncrementSleepInhibition();

	if (source) {
		obs_source_inc_showing(source);
	}

	ready = true;

	show();

	// We need it here to allow keyboard input in X11 to listen to Escape
	activateWindow();
}

OBSProjector::~OBSProjector()
{
	sigs.clear();

	obs_display_remove_draw_callback(GetDisplay(), OBSRender, this);

	OBSSource source = GetSource();
	if (source) {
		obs_source_dec_showing(source);
	}

	App()->DecrementSleepInhibition();
}

void OBSProjector::SetMonitor(int monitor)
{
	savedMonitor = monitor;
	setGeometry(QGuiApplication::screens()[monitor]->geometry());
	showFullScreen();
	SetHideCursor();
}

void OBSProjector::SetHideCursor()
{
	if (savedMonitor == -1) {
		return;
	}

	bool hideCursor = config_get_bool(App()->GetUserConfig(), "BasicWindow", "HideProjectorCursor");

	if (hideCursor) {
		setCursor(Qt::BlankCursor);
	} else {
		setCursor(Qt::ArrowCursor);
	}
}

void OBSProjector::OBSRender(void *data, uint32_t cx, uint32_t cy)
{
	OBSProjector *window = static_cast<OBSProjector *>(data);

	if (!window->ready) {
		return;
	}

	/* THE PROGRAM PROJECTOR RENDERS THE MAIN TEXTURE AND NOTHING ELSE.
	 *
	 * Upstream's OBSRender resolved window->weakSource first and only fell through to
	 * obs_render_main_texture() when there wasn't one — that branch is what made the scene and
	 * source projectors possible, and it is what put an unmasked camera on a display. There is no
	 * source path any more: the only thing this window can draw is the composite that is already
	 * being broadcast, mask included, so by construction it exposes nothing new. */
	struct obs_video_info ovi;
	obs_get_video_info(&ovi);

	const uint32_t targetCX = ovi.base_width;
	const uint32_t targetCY = ovi.base_height;
	int x, y;
	float scale;

	GetScaleAndCenterPos(targetCX, targetCY, cx, cy, x, y, scale);

	const int newCX = int(scale * float(targetCX));
	const int newCY = int(scale * float(targetCY));

	startRegion(x, y, newCX, newCY, 0.0f, float(targetCX), 0.0f, float(targetCY));
	obs_render_main_texture();
	endRegion();
}

void OBSProjector::OBSSourceRenamed(void *data, calldata_t *params)
{
	OBSProjector *window = static_cast<OBSProjector *>(data);
	QString oldName = QString::fromUtf8(calldata_string(params, "prev_name"));
	QString newName = QString::fromUtf8(calldata_string(params, "new_name"));

	QMetaObject::invokeMethod(window, &OBSProjector::RenameProjector, oldName, newName);
}

void OBSProjector::OBSSourceDestroyed(void *data, calldata_t *)
{
	OBSProjector *window = static_cast<OBSProjector *>(data);
	QMetaObject::invokeMethod(window, &OBSProjector::EscapeTriggered);
}

void OBSProjector::mouseDoubleClickEvent(QMouseEvent *event)
{
	/* Upstream used this to cut a multiview cell to program on a double click. There is no
	 * multiview and there is no preview/program split, so a double click on the program window
	 * does nothing — and MUST do nothing: this window shows what is already on air. */
	OBSQTDisplay::mouseDoubleClickEvent(event);
}

void OBSProjector::mousePressEvent(QMouseEvent *event)
{
	OBSQTDisplay::mousePressEvent(event);

	if (event->button() == Qt::RightButton) {
		QMenu *projectorMenu = new QMenu(QTStr("Fullscreen"));
		OBSBasic::AddProjectorMenuMonitors(projectorMenu, this, &OBSProjector::OpenFullScreenProjector);

		QMenu popup(this);
		popup.addMenu(projectorMenu);

		if (GetMonitor() > -1) {
			popup.addAction(QTStr("Windowed"), this, &OBSProjector::OpenWindowedProjector);

		} else if (!this->isMaximized()) {
			popup.addAction(QTStr("Projector.ResizeWindowToContent"), this, &OBSProjector::ResizeToContent);
		}

		QAction *alwaysOnTopButton = new QAction(QTStr("Basic.MainMenu.View.AlwaysOnTop"), this);
		alwaysOnTopButton->setCheckable(true);
		alwaysOnTopButton->setChecked(isAlwaysOnTop);

		connect(alwaysOnTopButton, &QAction::toggled, this, &OBSProjector::AlwaysOnTopToggled);

		popup.addAction(alwaysOnTopButton);

		popup.addAction(QTStr("Close"), this, &OBSProjector::EscapeTriggered);
		popup.exec(QCursor::pos());
	}
}

void OBSProjector::EscapeTriggered()
{
	OBSBasic *main = OBSBasic::Get();
	main->DeleteProjector(this);
}

void OBSProjector::UpdateProjectorTitle(QString name)
{
	UNUSED_PARAMETER(name);
	setWindowTitle(QTStr("Projector.Title") + " - " + QTStr("Projector.Title.Program"));
}

OBSSource OBSProjector::GetSource()
{
	return OBSGetStrongRef(weakSource);
}

ProjectorType OBSProjector::GetProjectorType()
{
	return type;
}

int OBSProjector::GetMonitor()
{
	return savedMonitor;
}

void OBSProjector::RenameProjector(QString oldName, QString newName)
{
	if (oldName == newName) {
		return;
	}

	UpdateProjectorTitle(newName);
}

void OBSProjector::OpenFullScreenProjector()
{
	if (!isFullScreen()) {
		prevGeometry = geometry();
	}

	int monitor = sender()->property("monitor").toInt();
	SetMonitor(monitor);

	OBSSource source = GetSource();
	UpdateProjectorTitle(QT_UTF8(obs_source_get_name(source)));
}

void OBSProjector::OpenWindowedProjector()
{
	showFullScreen();
	showNormal();
	setCursor(Qt::ArrowCursor);

	if (!prevGeometry.isNull()) {
		setGeometry(prevGeometry);
	} else {
		resize(480, 270);
	}

	savedMonitor = -1;

	OBSSource source = GetSource();
	UpdateProjectorTitle(QT_UTF8(obs_source_get_name(source)));
}

void OBSProjector::ResizeToContent()
{
	OBSSource source = GetSource();
	uint32_t targetCX;
	uint32_t targetCY;
	int x, y, newX, newY;
	float scale;

	if (source) {
		targetCX = std::max(obs_source_get_width(source), 1u);
		targetCY = std::max(obs_source_get_height(source), 1u);
	} else {
		struct obs_video_info ovi;
		obs_get_video_info(&ovi);
		targetCX = ovi.base_width;
		targetCY = ovi.base_height;
	}

	QSize size = this->size();
	GetScaleAndCenterPos(targetCX, targetCY, size.width(), size.height(), x, y, scale);

	newX = size.width() - (x * 2);
	newY = size.height() - (y * 2);
	resize(newX, newY);
}

void OBSProjector::AlwaysOnTopToggled(bool isAlwaysOnTop)
{
	SetIsAlwaysOnTop(isAlwaysOnTop, true);
}

void OBSProjector::closeEvent(QCloseEvent *event)
{
	EscapeTriggered();
	event->accept();
}

bool OBSProjector::IsAlwaysOnTop() const
{
	return isAlwaysOnTop;
}

bool OBSProjector::IsAlwaysOnTopOverridden() const
{
	return isAlwaysOnTopOverridden;
}

void OBSProjector::SetIsAlwaysOnTop(bool isAlwaysOnTop, bool isOverridden)
{
	this->isAlwaysOnTop = isAlwaysOnTop;
	this->isAlwaysOnTopOverridden = isOverridden;

	SetAlwaysOnTop(this, isAlwaysOnTop);
}

void OBSProjector::ScreenRemoved(QScreen *screen)
{
	if (GetMonitor() < 0) {
		return;
	}

	if (screen == this->screen()) {
		EscapeTriggered();
	}
}
