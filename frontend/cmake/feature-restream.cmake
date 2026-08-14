# Restream API connection -- the chat / stream-info / channels docks.
#
# ============================================================================================
# QCi: HOW TO TURN THE RESTREAM DOCKS ON
# ============================================================================================
# Restream is the only streaming destination this fork ships (see
# plugins/rtmp-services/data/services.json). Streaming to it needs nothing from this file:
# picking "Restream.io" in Settings -> Stream and pasting a stream key works in a plain build.
#
# This gate is only for the *optional* extra: `RESTREAM_ENABLED` compiles
# oauth/RestreamAuth.cpp, which adds a "Connect Account" button that logs into Restream over
# OAuth, pulls the stream key automatically, and docks three CEF browser panels inside the app:
#   * Chat            -> https://restream.io/chat-application
#   * Stream Info     -> https://restream.io/titles/embed
#   * Channels        -> https://restream.io/channel/embed
#
# It is gated on TWO variables, both read from the environment by CMakePresets.json (see the
# "environmentVars" preset, which maps them with $penv{...}), so export them in the shell
# before configuring:
#
#   RESTREAM_CLIENTID  The OAuth client id of a Restream application, XOR-obfuscated -- NOT the
#                      plain id. At runtime RestreamAuth.cpp calls
#                      deobfuscate_str(&client_id[0], RESTREAM_HASH), so the string baked in
#                      here must already be the obfuscated form that RESTREAM_HASH decodes back
#                      into the real id. (The obfuscation is the 4-bit XOR in
#                      frontend/utility/obf.c; it is anti-scraping, not security.)
#
#   RESTREAM_HASH      The 64-bit XOR key used to deobfuscate the above, written as bare hex
#                      digits with no 0x prefix -- ui-config.h.in prepends the 0x. Must match
#                      "^(0|[a-fA-F0-9]+)$" or the feature stays off. "0" means "not set".
#
# Get the client id by registering your own application in the Restream developer portal
# (https://restream.io -> account/developer settings); Restream issues the client id and
# secret to you. Do not commit either value -- pass them through the environment only:
#
#   export RESTREAM_CLIENTID='<obfuscated client id>'
#   export RESTREAM_HASH='<hex xor key, no 0x>'
#   cmake --preset macos            # or the fork's normal configure line
#
# A third condition is structural: TARGET OBS::browser-panels must exist, i.e. the build must
# include the CEF browser panels. Without them there is nowhere to put the docks and the
# feature stays off no matter what the two variables say.
#
# PRIVACY NOTE, since this fork just closed its last outbound fetch (see
# plugins/rtmp-services/CMakeLists.txt): enabling this deliberately opens new ones. The OAuth
# redirect and token exchange go through OAUTH_BASE_URL, which defaults to
# https://auth.obsproject.com/ (frontend/CMakeLists.txt), channel info comes from
# https://api.restream.io/v2/user/streamKey, and the three docks are live restream.io pages
# rendered by CEF. Leaving RESTREAM_CLIENTID unset keeps all of that out of the binary.
# ============================================================================================
if(RESTREAM_CLIENTID AND RESTREAM_HASH MATCHES "^(0|[a-fA-F0-9]+)$" AND TARGET OBS::browser-panels)
  target_sources(obs-studio PRIVATE oauth/RestreamAuth.cpp oauth/RestreamAuth.hpp)
  target_enable_feature(obs-studio "Restream API connection" RESTREAM_ENABLED)
else()
  target_disable_feature(obs-studio "Restream API connection")
  set(RESTREAM_CLIENTID "")
  set(RESTREAM_HASH "0")
endif()
