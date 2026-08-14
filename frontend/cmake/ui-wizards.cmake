# THE AUTO-CONFIGURATION WIZARD IS DELETED, and this file is kept as the note saying so because
# frontend/CMakeLists.txt includes it by name.
#
# It was a first-run tool for a machine nobody had configured yet: pick streaming or recording,
# pick a service, run a BANDWIDTH TEST against that service's ingest, then rewrite the profile's
# video and output settings with what it measured. Every one of those steps is wrong here. This rig
# has a configured profile, an encoder chosen for it, a virtual camera PINNED to a named scene, a
# 26-scene collection and a privacy watchdog that reads program scene names — and the wizard would
# have rewritten the output settings underneath all of it, from a test run against a live service,
# with no undo. It is a hazard on this machine, not a convenience.
#
# TestMode.hpp went with it: its whole job was to shut the preview down and swap the video settings
# out from under the running graphics thread while the test ran, and it had no other caller.
