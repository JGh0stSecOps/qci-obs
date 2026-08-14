cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_PLUGINS "Enable building OBS plugins" ON)

if(NOT ENABLE_PLUGINS)
  set_property(GLOBAL APPEND PROPERTY OBS_FEATURES_DISABLED "Plugin Support")
  return()
endif()

set_property(GLOBAL APPEND PROPERTY OBS_FEATURES_ENABLED "Plugin Support")

macro(check_obs_browser)
  if((OS_WINDOWS AND CMAKE_VS_PLATFORM_NAME MATCHES "(ARM64|x64)") OR OS_MACOS OR OS_LINUX)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/obs-browser/CMakeLists.txt")
      message(FATAL_ERROR "Required submodule 'obs-browser' not available.")
    else()
      add_subdirectory(obs-browser)
    endif()
  else()
    add_custom_target(obs-browser)
    target_disable(obs-browser)
  endif()
endmacro()

macro(check_obs_websocket)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/obs-websocket/CMakeLists.txt")
    message(FATAL_ERROR "Required submodule 'obs-websocket' not available.")
  else()
    add_subdirectory(obs-websocket)
  endif()
endmacro()

# Add plugins in alphabetical order to retain order in IDE projects
#
# The AJA and DeckLink families (aja, aja-output-ui, decklink, decklink-captions,
# decklink-output-ui) were removed from this fork. They drive PCIe/Thunderbolt broadcast capture
# cards; this is a personal, never-distributed build whose capture chain is webcam + screen capture
# + browser sources. Unlike the Windows/Linux-gated plugins below, those five were built FOR MACOS,
# so they cost every local build real compile time. obs-libfdk went with them: it is a second AAC
# encoder next to coreaudio-encoder, which is the one macOS actually uses.
add_obs_plugin(coreaudio-encoder PLATFORMS WINDOWS MACOS)
add_obs_plugin(
    frontend-tools
    PLATFORMS WINDOWS MACOS LINUX
    WITH_MESSAGE
)
add_obs_plugin(image-source)
add_obs_plugin(linux-alsa PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(linux-capture PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(linux-jack PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(linux-pipewire PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(linux-pulseaudio PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(linux-v4l2 PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(mac-avcapture PLATFORMS MACOS)
add_obs_plugin(mac-capture PLATFORMS MACOS)
add_obs_plugin(mac-syphon PLATFORMS MACOS)
add_obs_plugin(mac-videotoolbox PLATFORMS MACOS)
add_obs_plugin(mac-virtualcam PLATFORMS MACOS)
add_obs_plugin(nv-filters PLATFORMS WINDOWS)

check_obs_browser()

add_obs_plugin(obs-ffmpeg)
add_obs_plugin(obs-filters)
add_obs_plugin(obs-nvenc PLATFORMS WINDOWS LINUX ARCHITECTURES x64 x86_64)
add_obs_plugin(obs-outputs)
add_obs_plugin(
    obs-qsv11
    PLATFORMS WINDOWS LINUX
    ARCHITECTURES x64 x86_64
)
add_obs_plugin(obs-text PLATFORMS WINDOWS)
add_obs_plugin(obs-transitions)
add_obs_plugin(
    obs-vst
    PLATFORMS WINDOWS MACOS LINUX
    WITH_MESSAGE
)
add_obs_plugin(obs-webrtc)

check_obs_websocket()

add_obs_plugin(obs-x264)
add_obs_plugin(oss-audio PLATFORMS FREEBSD OPENBSD)

# QCi: fork-only plugins. Out of alphabetical order on purpose — upstream's comment above asks for
# alphabetical, and "qci-" would sort between obs-x264 and rtmp-services, which is exactly where a
# merge conflict lands on every rebase against upstream. Keeping fork-only plugins in one block
# after the upstream list makes an upstream merge a no-op here. See
# plugins/qci-vision-person/CMakeLists.txt for why these are in-tree rather than installed
# alongside the third-party plugins.
#
# qci-audio-unit hosts AudioUnit effects — the macOS-native effect format. It sits ALONGSIDE
# obs-vst rather than replacing it: the claim that VST2 hosting is inert on Apple Silicon was
# checked against this machine and does not hold (both installed VST2 bundles are arm64-native).
# See plugins/qci-audio-unit/qci-audio-unit.m for the measurement.
add_obs_plugin(qci-vision-person PLATFORMS MACOS)
add_obs_plugin(qci-audio-unit PLATFORMS MACOS)

add_obs_plugin(rtmp-services)
add_obs_plugin(sndio PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(text-freetype2)
add_obs_plugin(vlc-video WITH_MESSAGE)
add_obs_plugin(win-capture PLATFORMS WINDOWS)
add_obs_plugin(win-dshow PLATFORMS WINDOWS)
add_obs_plugin(win-wasapi PLATFORMS WINDOWS)
