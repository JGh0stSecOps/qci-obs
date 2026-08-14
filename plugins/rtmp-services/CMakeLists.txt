cmake_minimum_required(VERSION 3.28...3.30)

# QCi: OFF by default in this fork, and it must stay OFF.
#
# This was the LAST outbound fetch that fired automatically, on every launch, from an
# unconditionally-compiled code path. With it ON, obs_module_load() hits RTMP_SERVICES_URL
# below on every start and overwrites data/services.json in the module cache with
# obsproject.com's full 84-service list -- which would silently undo the Restream-only trim
# of that file and put every other ingest back in the dropdown. Turning the file down to one
# service is only meaningful while nothing re-downloads it.
#
# Precisely, since "last phone-home in the tree" is worth stating exactly. What is left,
# and why each is not this: the Sparkle updater and the What's New feed are gone outright
# (frontend/cmake/feature-sparkle.cmake); win-capture's identical updater is Windows-only and
# this fork builds macOS; every GetRemoteFile() caller left in frontend/ is either OAuth
# (build-gated on a *_CLIENTID that is unset) or the log upload, which only runs when the
# operator clicks Upload. ONE unconditional fetch does survive, and it is not automatic:
# frontend/dialogs/QCiAbout.cpp fetches https://obsproject.com/patreon/about-box.json when
# the Help -> About dialog is opened. That one is still open; nothing else is.
#
# The operator streams to Restream and nothing else, so there is nothing to update:
# data/services.json ships the 21 Restream ingest servers, and Custom RTMP does not come
# from this file at all (it is rtmp-custom.c, registered independently of services.json).
#
# WHY set(... FORCE) AND NOT option(). This used to be `option(ENABLE_SERVICE_UPDATES ... ON)`,
# and build_macos/CMakeCache.txt already carries `ENABLE_SERVICE_UPDATES:BOOL=ON` from an earlier
# configure. option() does not overwrite a cache entry that already exists, so flipping its
# default to OFF would have changed nothing at all for the tree the operator actually builds:
# the next build would still have compiled the fetch in, and the first launch would still have
# pulled all 84 services back over data/services.json. FORCE is what makes the value stick
# without needing a wiped build directory. RTMP_SERVICES_URL just below uses FORCE for the same
# reason.
#
# The consequence, deliberately: -DENABLE_SERVICE_UPDATES=ON on the command line will NOT turn
# this back on. Re-enabling means editing this line -- which is the point. Fail closed.
set(
  ENABLE_SERVICE_UPDATES
  OFF
  CACHE BOOL
  "Checks for service updates (QCi: forced off - see comment above)"
  FORCE
)

# Only ever contacted when ENABLE_SERVICE_UPDATES is ON, which in this fork it is not.
set(
  RTMP_SERVICES_URL
  "https://obsproject.com/obs2_update/rtmp-services"
  CACHE STRING
  "Default services package URL"
  FORCE
)
mark_as_advanced(RTMP_SERVICES_URL)

if(NOT TARGET OBS::file-updater)
  add_subdirectory("${CMAKE_SOURCE_DIR}/shared/file-updater" "${CMAKE_BINARY_DIR}/shared/file-updater")
endif()

find_package(jansson REQUIRED)

add_library(rtmp-services MODULE)
add_library(OBS::rtmp-services ALIAS rtmp-services)

# QCi: the whole service-specific/ directory is gone. Those files existed only to resolve
# ingest servers for Twitch, Amazon IVS, Dacast, Nimo TV and SHOWROOM -- none of which are
# in data/services.json any more, so none of them could ever be selected. Deleting them also
# removes their own direct HTTP calls (ingest.twitch.tv, dacast, nimotv, showroom).
# rtmp-custom.c stays: Custom RTMP is a separately registered service and does not read
# data/services.json.
target_sources(
  rtmp-services
  PRIVATE
    rtmp-common.c
    rtmp-custom.c
    rtmp-format-ver.h
    rtmp-services-main.c
)

target_compile_definitions(
  rtmp-services
  PRIVATE SERVICES_URL="${RTMP_SERVICES_URL}" $<$<BOOL:${ENABLE_SERVICE_UPDATES}>:ENABLE_SERVICE_UPDATES>
)

target_link_libraries(rtmp-services PRIVATE OBS::libobs OBS::file-updater jansson::jansson)

# Remove once jansson has been fixed on obs-deps
target_link_options(rtmp-services PRIVATE $<$<PLATFORM_ID:Windows>:/IGNORE:4098>)

if(OS_WINDOWS)
  configure_file(cmake/windows/obs-module.rc.in rtmp-services.rc)
  target_sources(rtmp-services PRIVATE rtmp-services.rc)
endif()

set_target_properties_obs(rtmp-services PROPERTIES FOLDER plugins PREFIX "")
