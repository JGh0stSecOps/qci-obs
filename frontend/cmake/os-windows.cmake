# NOTE: This fork is macOS-only and the Windows frontend updater is gone (frontend/updater/, the
# QCiUpdate dialog, AutoUpdateThread, and the blake2/MbedTLS signature stack that fed them). What
# remains here is the non-update Windows platform glue, kept so an upstream merge still lands
# cleanly on this file rather than conflicting over a wholesale deletion. A Windows build of this
# fork would need the updater restored from git history, or those code paths stubbed out.

if(NOT TARGET OBS::w32-pthreads)
  add_subdirectory("${CMAKE_SOURCE_DIR}/deps/w32-pthreads" "${CMAKE_BINARY_DIR}/deps/w32-pthreads")
endif()

find_package(Detours REQUIRED)

configure_file(cmake/windows/qcis.rc.in qcis.rc)

target_sources(
  obs-studio
  PRIVATE
    cmake/windows/qcis.manifest
    qcis.rc
    utility/CrashHandler_Windows.cpp
    utility/NativeEventFilter_Windows.cpp
    utility/platform-windows.cpp
    utility/system-info-windows.cpp
    utility/win-dll-blocklist.c
)

target_link_libraries(obs-studio PRIVATE crypt32 OBS::w32-pthreads Detours::Detours)

target_compile_definitions(obs-studio PRIVATE PSAPI_VERSION=2)

target_link_options(obs-studio PRIVATE /IGNORE:4099 $<$<CONFIG:DEBUG>:/NODEFAULTLIB:MSVCRT>)

set_property(TARGET obs-studio APPEND PROPERTY AUTORCC_OPTIONS --format-version 1)

set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT obs-studio)
set_target_properties(
  obs-studio
  PROPERTIES
    WIN32_EXECUTABLE TRUE
    VS_DEBUGGER_COMMAND "${CMAKE_BINARY_DIR}/rundir/$<CONFIG>/bin/64bit/$<TARGET_FILE_NAME:obs-studio>"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/rundir/$<CONFIG>/bin/64bit"
)
