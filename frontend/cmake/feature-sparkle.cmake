# ⚠️ THIS FORK MUST NEVER SHIP SPARKLE, and the reason is not policy but mechanics.
# Sparkle replaces the *host bundle* in place. Upstream's appcast (updates_$(ARCHS)_v2.xml) and
# upstream's EdDSA public key were both baked into this tree's macOS preset, so the very first
# accepted "update" would have downloaded official OBS Studio, verified it correctly against
# upstream's key — the signature check PASSES, that is the point — and overwritten QCi-Studio.app
# with it. The operator would have lost the fork, silently, to a legitimate-looking update. This
# is the same class of mistake as the bundle-identifier incident recorded in
# cmake/macos/helpers.cmake: borrowing the parent project's identity hands the parent project
# control over this machine's app.
#
# SPARKLE_APPCAST_URL and SPARKLE_PUBLIC_KEY are therefore empty in CMakePresets.json (JSON has
# no comments, hence this note here).
#
# The updater sources themselves are now GONE, not merely disabled. MacUpdateThread, QCiSparkle,
# QCiUpdateDelegate and the branch-list machinery were deleted along with the What's New feed they
# shared (MacUpdateThread called FetchAndVerifyFile, which lived in WhatsNewInfoThread.cpp). This
# is a personal build that is installed by copying a bundle; it has no appcast to check and no
# business phoning home.
#
# Setting the two cache variables can therefore no longer produce a working updater, so it is a
# hard error rather than a silently half-configured build. Restoring Sparkle means restoring those
# sources from git history AND minting this fork's own appcast and signing key — never upstream's.
if(SPARKLE_APPCAST_URL OR SPARKLE_PUBLIC_KEY)
  message(
    FATAL_ERROR
      "SPARKLE_APPCAST_URL / SPARKLE_PUBLIC_KEY are set, but this fork's Sparkle updater sources "
      "were removed. Clear both variables, or restore the updater from git history together with "
      "an appcast and EdDSA key belonging to QCi-Studio — never upstream OBS's."
  )
endif()

target_disable_feature(obs-studio "Sparkle updater")
