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
# no comments, hence this note here). Empty is falsy in CMake, so the else() branch below is the
# live path: no Sparkle framework, no updater sources, no SU* keys in Info.plist. Do not restore
# them unless QCi-Studio grows its own appcast signed with its own key.
if(SPARKLE_APPCAST_URL AND SPARKLE_PUBLIC_KEY)
  find_library(SPARKLE Sparkle)
  mark_as_advanced(SPARKLE)
  target_sources(
    obs-studio
    PRIVATE
      utility/MacUpdateThread.cpp
      utility/MacUpdateThread.hpp
      utility/QCiSparkle.hpp
      utility/QCiSparkle.mm
      utility/QCiUpdateDelegate.h
      utility/QCiUpdateDelegate.mm
  )
  set_source_files_properties(utility/QCiSparkle.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)

  target_link_libraries(obs-studio PRIVATE "$<LINK_LIBRARY:FRAMEWORK,${SPARKLE}>")

  if(OBS_BETA GREATER 0 OR OBS_RELEASE_CANDIDATE GREATER 0)
    set(SPARKLE_UPDATE_INTERVAL 3600) # 1 hour
  else()
    set(SPARKLE_UPDATE_INTERVAL 86400) # 24 hours
  endif()

  target_enable_feature(obs-studio "Sparkle updater" ENABLE_SPARKLE_UPDATER)

  include(cmake/feature-macos-update.cmake)
else()
  set(SPARKLE_UPDATE_INTERVAL 0) # Set anything that's not an empty integer
  target_disable_feature(obs-studio "Sparkle updater")
endif()
