# OBS CMake bootstrap module

include_guard(GLOBAL)

# Map fallback configurations for optimized build configurations
# gersemi: off
set(
  CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO
    RelWithDebInfo
    Release
    MinSizeRel
    None
    ""
)
set(
  CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL
    MinSizeRel
    Release
    RelWithDebInfo
    None
    ""
)
set(
  CMAKE_MAP_IMPORTED_CONFIG_RELEASE
    Release
    RelWithDebInfo
    MinSizeRel
    None
    ""
)
# gersemi: on

# Prohibit in-source builds
if("${CMAKE_CURRENT_BINARY_DIR}" STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}")
  message(
    FATAL_ERROR
    "In-source builds of QCi Studio are not supported. "
    "Specify a build directory via 'cmake -S <SOURCE DIRECTORY> -B <BUILD_DIRECTORY>' instead."
  )
  file(REMOVE_RECURSE "${CMAKE_CURRENT_SOURCE_DIR}/CMakeCache.txt" "${CMAKE_CURRENT_SOURCE_DIR}/CMakeFiles")
endif()

# Set default global project variables
set(OBS_COMPANY_NAME "Zoetic Solutions")
# THE product name the user reads. This is the ONLY definition of it in the tree: every window
# title, log banner, crash dialog and tray tooltip is built from this via the OBS_PRODUCT_NAME
# macro that frontend/cmake/templates/ui-config.h.in configures out of it. Do not re-spell
# "QCi Studio" as a literal in C++ — change it here and the whole app follows.
set(OBS_PRODUCT_NAME "QCi Studio")
# Name of the per-user data directory: "~/Library/Application Support/<dir>" on macOS,
# "%APPDATA%\<dir>" on Windows, "$XDG_CONFIG_HOME/<dir>" on Linux. Upstream hardcodes the literal
# "obs-studio" at ~40 call sites, so any fork silently reads and writes the user's stock OBS state.
# This fork is installed alongside a production OBS that drives a live stream, and before this
# variable existed it overwrote that installation's global.ini. OBS_PRODUCT_NAME cannot be reused
# here: it is only a display string (it contains a space) and is not path-safe. Set at bootstrap
# scope so libobs (which string-matches the directory in its legacy obs-browser guard) sees it too.
set(OBS_USER_DATA_DIR "qci-studio")
# Package metadata only (CPACK_PACKAGE_VENDOR / CPACK_PACKAGE_HOMEPAGE_URL, pulled in solely by
# cmake/linux/defaults.cmake and cmake/windows/defaults.cmake). This is the fork's own home, not
# upstream's. Do NOT reuse it for anything that must fetch a real upstream resource — the crash
# privacy notice, the What's New feed and the Sparkle appcast all still point at obsproject.com
# on purpose, because that is where those resources actually live.
set(OBS_WEBSITE "https://github.com/JGh0stSecOps/qci-studio")
set(OBS_COMMENTS "QCi Studio: free and open source software for video recording and live streaming")
# Upstream's copyright line is not branding and must not be dropped — this is a GPLv2 fork and
# Lain Bailey holds copyright on the code it is built from. The fork's line is appended, not
# substituted.
set(OBS_LEGAL_COPYRIGHT "(C) Lain Bailey; QCi Studio fork (C) Zoetic Solutions")
set(OBS_CMAKE_VERSION 3.0.0)

# Configure default version strings
set(_obs_default_version "0" "0" "1")
set(_obs_release_candidate 0)
set(_obs_beta 0)

# Add common module directories to default search path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/common" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/finders")

include(policies NO_POLICY_SCOPE)
include(versionconfig)
include(buildnumber)
include(osconfig)

# Allow selection of common build types via UI
if(NOT CMAKE_GENERATOR MATCHES "(Xcode|Visual Studio .+)")
  if(NOT CMAKE_BUILD_TYPE)
    set(
      CMAKE_BUILD_TYPE
      "RelWithDebInfo"
      CACHE STRING
      "QCi Studio build type [Release, RelWithDebInfo, Debug, MinSizeRel]"
      FORCE
    )
    set_property(
      CACHE CMAKE_BUILD_TYPE
      PROPERTY STRINGS Release RelWithDebInfo Debug MinSizeRel
    )
  endif()
endif()

# Enable default inclusion of targets' source and binary directory
set(CMAKE_INCLUDE_CURRENT_DIR TRUE)
