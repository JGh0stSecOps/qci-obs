if(NOT TARGET OBS::qt-vertical-scroll-area)
  add_subdirectory(
    "${CMAKE_SOURCE_DIR}/shared/qt/vertical-scroll-area"
    "${CMAKE_BINARY_DIR}/shared/qt/vertical-scroll-area"
  )
endif()

target_link_libraries(obs-studio PRIVATE OBS::qt-vertical-scroll-area)

target_sources(
  obs-studio
  PRIVATE
    widgets/AudioMixer.cpp
    widgets/AudioMixer.hpp
    widgets/ColorSelect.cpp
    widgets/ColorSelect.hpp
    widgets/QCiBasic.cpp
    widgets/QCiBasic.hpp
    widgets/QCiBasic_Browser.cpp
    widgets/QCiBasic_Canvases.cpp
    widgets/QCiBasic_Clipboard.cpp
    widgets/QCiBasic_ContextToolbar.cpp
    widgets/QCiBasic_Docks.cpp
    widgets/QCiBasic_Dropfiles.cpp
    widgets/QCiBasic_Hotkeys.cpp
    widgets/QCiBasic_Icons.cpp
    widgets/QCiBasic_MainControls.cpp
    widgets/QCiBasic_OutputHandler.cpp
    widgets/QCiBasic_Preview.cpp
    widgets/QCiBasic_Profiles.cpp
    widgets/QCiBasic_Projectors.cpp
    widgets/QCiBasic_Recording.cpp
    widgets/QCiBasic_ReplayBuffer.cpp
    widgets/QCiBasic_SceneCollections.cpp
    widgets/QCiBasic_SceneItems.cpp
    widgets/QCiBasic_Scenes.cpp
    widgets/QCiBasic_Screenshots.cpp
    widgets/QCiBasic_Service.cpp
    widgets/QCiBasic_StatusBar.cpp
    widgets/QCiBasic_Streaming.cpp
    widgets/QCiBasic_StudioMode.cpp
    widgets/QCiBasic_SysTray.cpp
    widgets/QCiBasic_Transitions.cpp
    widgets/QCiBasic_VirtualCam.cpp
    widgets/QCiBasic_YouTube.cpp
    widgets/QCiBasicControls.cpp
    widgets/QCiBasicControls.hpp
    widgets/QCiBasicPreview.cpp
    widgets/QCiBasicPreview.hpp
    widgets/QCiBasicStats.cpp
    widgets/QCiBasicStats.hpp
    widgets/QCiBasicStatusBar.cpp
    widgets/QCiBasicStatusBar.hpp
    widgets/QCiMainWindow.hpp
    # ── THE TWO PERSISTENT STRIPS ──────────────────────────────────────────────────────────────
    # Neither is a dock and neither can become one. They are window chrome: a QToolBar pinned to the
    # top area (which in a QMainWindow sits OUTSIDE the four dock areas, so it spans the full width
    # above both columns) and a widget that replaces the layout inside the existing status bar.
    # Between them they are what makes this window unmistakable before a single label is read, and
    # they are why the flight strip is NEW surface rather than a restyle — QCiBasic.ui has exactly
    # two QToolBars, both of them inside docks, and no application toolbar at all.
    widgets/QCiCommandBar.cpp
    widgets/QCiCommandBar.hpp
    widgets/QCiFlightStrip.cpp
    widgets/QCiFlightStrip.hpp
    widgets/QCiProjector.cpp
    widgets/QCiProjector.hpp
    widgets/QCiQTDisplay.cpp
    widgets/QCiQTDisplay.hpp
    widgets/StatusBarWidget.cpp
    widgets/StatusBarWidget.hpp
)
