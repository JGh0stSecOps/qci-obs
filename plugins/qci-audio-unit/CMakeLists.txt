cmake_minimum_required(VERSION 3.28...3.30)

# qci-audio-unit — AudioUnit (v2) effect hosting, first-party.
#
# AudioUnit is the macOS-native effect format and the one the operator's wanted mic-chain
# effects ship in. This target makes those hostable as an OBS audio filter.
#
# ⚠️ This does NOT exist because obs-vst is broken. The brief asserted VST2 hosting is inert on
# Apple Silicon; that was measured and is false here — both VST2 bundles installed on this
# machine are arm64-native, and obs-vst still builds for macOS in this tree. See the header
# comment in qci-audio-unit.m for the evidence. AU is an additional route, not a replacement.
#
# Two halves, split along a test boundary rather than a language one:
#
#   qci-au-format.c   pure C. No libobs, no Objective-C, no live AudioUnit. Holds the parts that
#                     are easy to get quietly wrong — the non-interleaved ASBD, AUChannelInfo
#                     wildcard negotiation, the persisted component identity, the ClassInfo
#                     round trip, and the predicate deciding when a running unit must be rebuilt —
#                     so they can be executed by a test binary on a machine with no audio device
#                     and none of the third-party effects installed.
#   qci-audio-unit.m  the OBS filter: discovery, instantiation, the realtime render path, and
#                     the Cocoa editor window.

add_library(qci-audio-unit MODULE)
add_library(OBS::audio-unit ALIAS qci-audio-unit)

target_sources(qci-audio-unit PRIVATE qci-au-format.c qci-au-format.h qci-audio-unit.m)

target_link_libraries(
  qci-audio-unit
  PRIVATE
    OBS::libobs
    # AudioComponentFindNext / AudioUnitRender / the kAudioUnitProperty_* surface. AudioUnit is
    # a subframework of AudioToolbox on macOS, so this one entry covers both.
    "$<LINK_LIBRARY:FRAMEWORK,AudioToolbox.framework>"
    # AUGenericView — the fallback editor for units that publish no Cocoa view of their own.
    "$<LINK_LIBRARY:FRAMEWORK,CoreAudioKit.framework>"
    # NSWindow hosting the plug-in's view.
    "$<LINK_LIBRARY:FRAMEWORK,Cocoa.framework>"
    "$<LINK_LIBRARY:FRAMEWORK,CoreFoundation.framework>"
    "$<LINK_LIBRARY:FRAMEWORK,Foundation.framework>"
)

set_target_properties_obs(
  qci-audio-unit
  PROPERTIES FOLDER plugins
             PREFIX ""
             XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
             XCODE_ATTRIBUTE_CLANG_WARN_SUSPICIOUS_IMPLICIT_CONVERSION YES
             XCODE_ATTRIBUTE_GCC_WARN_SHADOW YES
)

if(CMAKE_VERSION VERSION_LESS_EQUAL 3.25.0)
  set_property(TARGET qci-audio-unit PROPERTY XCODE_LINK_BUILD_PHASE_MODE BUILT_ONLY)
endif()

# ── unit test ───────────────────────────────────────────────────────────────────────────────
#
# A standalone binary rather than a cmocka one. The tree DOES ship a cmocka harness at
# test/cmocka/, but it is unreachable: nothing ever calls add_subdirectory(test), so its
# ENABLE_UNIT_TESTS branch never runs, and CMocka is not installed on this machine anyway
# (`find_package(CMocka CONFIG REQUIRED)` would hard-fail the configure). The pattern followed
# here is the one already in the tree that works without an external dependency —
# plugins/obs-x264/cmake/x264-test.cmake: an add_executable plus add_test, asserting with a
# CHECK macro that exits non-zero.
#
# It links neither libobs nor AppKit, so `ctest` exercises it without OBS, without an audio
# device, and without any third-party AudioUnit being installed.
#
# ⚠️ This add_test only registers because enable_testing() is called in the ROOT CMakeLists
# BEFORE add_subdirectory(plugins). It used to be called after, which silently discarded every
# add_test() in every subdirectory — see the comment there.
add_executable(qci-au-format-test)

target_sources(qci-au-format-test PRIVATE qci-au-format-test.c qci-au-format.c)

target_include_directories(qci-au-format-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(
  qci-au-format-test
  PRIVATE
    "$<LINK_LIBRARY:FRAMEWORK,AudioToolbox.framework>"
    "$<LINK_LIBRARY:FRAMEWORK,CoreFoundation.framework>"
)

add_test(NAME qci-au-format COMMAND qci-au-format-test)
set_tests_properties(qci-au-format PROPERTIES LABELS qci)

set_target_properties(qci-au-format-test PROPERTIES FOLDER plugins/qci-audio-unit)
