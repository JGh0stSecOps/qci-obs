target_sources(obs-studio PRIVATE docks/QCiDock.cpp docks/QCiDock.hpp)

# THE OPERATOR PANEL. Built unconditionally and with no browser dependency: the panes are real Qt
# widgets, the one client is Qt::Network (already linked, see cmake/ui-qt.cmake), and the model is
# plain C++. The single #ifdef BROWSER_AVAILABLE left in the whole panel is the CHAT pane's embed —
# a live-chat widget is somebody else's page and there is no native protocol to render it, so
# without a browser panel that one pane prints the URL instead of showing it. Everything else,
# including PRIVACY HOLD, works in a build with no CEF at all.
target_sources(
  obs-studio
  PRIVATE
    docks/QCiRigClient.cpp
    docks/QCiRigClient.hpp
    docks/QCiRigDocks.cpp
    docks/QCiRigDocks.hpp
    docks/QCiRigModel.cpp
    docks/QCiRigModel.hpp
    docks/QCiRigPanes.cpp
    docks/QCiRigPanes.hpp
    docks/QCiRigUi.cpp
    docks/QCiRigUi.hpp
)

# THE OPERATOR SECTION AND THE SCENE RAIL. One dock each, both undismissable, both registered from
# the same table in QCiRigDocks.cpp. The operator section carries PRIVACY HOLD, the mask segment,
# BRB, MIRROR, the five pages and the stream transport; the rail carries scene switching. They are
# separate files rather than more of QCiRigPanes.cpp because neither is a QCiRigPane — the panes are
# the five PAGES, and these two are the chrome around them.
target_sources(
  obs-studio
  PRIVATE
    docks/QCiOperatorPane.cpp
    docks/QCiOperatorPane.hpp
    docks/QCiSceneRail.cpp
    docks/QCiSceneRail.hpp
)

# THE AUDIO STRIP. A dock in the operator's RUN layout rather than a pane inside the operator
# section, because audio is the failure mode a streamer cannot see and it therefore gets a permanent
# horizontal band rather than a page you have to be on. It talks to libobs directly — obs_volmeter,
# obs_source_muted, obs_source_get_monitoring_type — and to the rig not at all: the app IS OBS and
# owns its own audio graph, so routing these three facts through an HTTP poll would be a slower copy
# of something already in process.
target_sources(obs-studio PRIVATE docks/QCiAudioStrip.cpp docks/QCiAudioStrip.hpp)
