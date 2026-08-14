#pragma once

#include "QCiQTDisplay.hpp"

/* ONE PROJECTOR TYPE, AND THE VALUE IS PINNED.
 *
 * Source(0), Scene(1), StudioProgram(3) and Multiview(4) are gone — see the privacy note at the
 * top of QCiBasic_Projectors.cpp. Program keeps the integer 2 it has always had, because
 * SaveProjectors() writes the enumerator's VALUE into the scene collection and a renumbering would
 * silently reopen a saved "2" as something else on the next launch. OpenSavedProjector() drops any
 * other value rather than guessing.
 *
 * It was called Preview upstream. With studio mode gone there is no preview: this display renders
 * the program composite, which is what the name now says. */
enum class ProjectorType {
	Program = 2,
};

class OBSProjector : public OBSQTDisplay {
	Q_OBJECT

private:
	OBSWeakSourceAutoRelease weakSource;
	std::vector<OBSSignal> sigs;

	static void OBSRender(void *data, uint32_t cx, uint32_t cy);
	static void OBSSourceRenamed(void *data, calldata_t *params);
	static void OBSSourceDestroyed(void *data, calldata_t *params);

	void mousePressEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void closeEvent(QCloseEvent *event) override;

	bool isAlwaysOnTop;
	bool isAlwaysOnTopOverridden = false;
	int savedMonitor = -1;
	ProjectorType type = ProjectorType::Program;

	bool ready = false;

	void UpdateProjectorTitle(QString name);

	QRect prevGeometry;
	void SetMonitor(int monitor);

private slots:
	void EscapeTriggered();
	void OpenFullScreenProjector();
	void ResizeToContent();
	void OpenWindowedProjector();
	void AlwaysOnTopToggled(bool alwaysOnTop);
	void ScreenRemoved(QScreen *screen);
	void RenameProjector(QString oldName, QString newName);

public:
	OBSProjector(QWidget *widget, obs_source_t *source_, int monitor, ProjectorType type_);
	~OBSProjector();

	OBSSource GetSource();
	ProjectorType GetProjectorType();
	int GetMonitor();
	void SetHideCursor();

	bool IsAlwaysOnTop() const;
	bool IsAlwaysOnTopOverridden() const;
	void SetIsAlwaysOnTop(bool isAlwaysOnTop, bool isOverridden);
};
