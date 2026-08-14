cmake_minimum_required(VERSION 3.28...3.30)

# qci-vision-person — the QC vision privacy mask, folded in as a FIRST-PARTY module.
#
# This target replaces a standalone out-of-tree build (~/obs-vision-plugin) that linked
# "/Applications/OBS.app/Contents/Frameworks/libobs.framework/libobs" against a vendored
# obs-studio 32.2.1 header tree. Two reasons that could not simply be copied across:
#
#   1. It bound a load-bearing privacy filter to the STOCK app's libobs. Updating OBS moved
#      the ABI under it, with no build step to catch the drift.
#   2. QCi Studio never searches stock's plugin directory on purpose — see the comment at
#      frontend/widgets/QCiBasic.cpp:145-155. Copying the .plugin across would work until the
#      operator opened its properties, at which point it would write settings into the FORK's
#      plugin_config while the same bundle keeps writing into stock's.
#
# Built here it links OBS::libobs (this tree's own target), compiles against this tree's
# headers with no vendored obs-src/ or simde-src/, and is embedded into the app bundle at
# Contents/PlugIns/ by _bundle_dependencies() — the path libobs adds in
# obs-cocoa.m:add_default_module_paths(). Nothing to install, nothing to keep in sync.
#
# ⚠️ The filter id "vision_person_mask" and the display name "Person Mask (Vision)" in
# vision-person-mask.mm are a contract with the operator's live scene collection (4 instances)
# and with qci-rig/lib/provision.mjs. Do not rename either.

add_library(qci-vision-person MODULE)
add_library(OBS::vision-person ALIAS qci-vision-person)

target_sources(qci-vision-person PRIVATE vision-person-mask.mm qci-vision-frame.c qci-vision-frame.h)

target_include_directories(qci-vision-person PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(
  qci-vision-person
  PRIVATE
    OBS::libobs
    # vImageMin/Max_Planar8, vImageScale_Planar8, vImageScale_ARGB8888 — the mask despeckle and
    # the box downscale that IS the per-block mean.
    "$<LINK_LIBRARY:FRAMEWORK,Accelerate.framework>"
    "$<LINK_LIBRARY:FRAMEWORK,CoreFoundation.framework>"
    "$<LINK_LIBRARY:FRAMEWORK,CoreMedia.framework>"
    "$<LINK_LIBRARY:FRAMEWORK,CoreVideo.framework>"
    "$<LINK_LIBRARY:FRAMEWORK,Foundation.framework>"
    # VTPixelTransferSession* — the format conversion into and back out of BGRA.
    "$<LINK_LIBRARY:FRAMEWORK,VideoToolbox.framework>"
    # VNGeneratePersonSegmentationRequest. Linked normally rather than weakly: Vision.framework
    # has shipped since macOS 10.13 and CMAKE_OSX_DEPLOYMENT_TARGET here is 13.0, so the
    # framework is always present. The @available(macOS 12.0) guards in the source cover the
    # request class itself and are kept as belt-and-braces.
    "$<LINK_LIBRARY:FRAMEWORK,Vision.framework>"
)

set_target_properties_obs(
  qci-vision-person
  PROPERTIES FOLDER plugins
             PREFIX ""
             XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
             XCODE_ATTRIBUTE_CLANG_WARN_SUSPICIOUS_IMPLICIT_CONVERSION YES
             XCODE_ATTRIBUTE_GCC_WARN_SHADOW YES
)

if(CMAKE_VERSION VERSION_LESS_EQUAL 3.25.0)
  set_property(TARGET qci-vision-person PROPERTY XCODE_LINK_BUILD_PHASE_MODE BUILT_ONLY)
endif()

# ── unit test ───────────────────────────────────────────────────────────────────────────────
#
# Same pattern, and for the same reason, as plugins/qci-audio-unit's qci-au-format-test: a
# standalone binary plus add_test(), because the tree's cmocka harness at test/cmocka/ is never
# added by the root CMakeLists and CMocka is not installed here.
#
# It links neither libobs nor Vision. That is what makes it useful: the defect it guards is the
# handling of VIDEO_FORMAT_P010, a 10-bit camera format this Mac's cameras do not negotiate, so a
# test that needed a camera to emit it would be a test that never ran. libobs's headers are on
# the include path for `enum video_format` alone — header-only, nothing linked.
#
# ⚠️ add_test() only registers because enable_testing() is called in the ROOT CMakeLists BEFORE
# add_subdirectory(plugins).
add_executable(qci-vision-frame-test)

target_sources(qci-vision-frame-test PRIVATE qci-vision-frame-test.c qci-vision-frame.c)

target_include_directories(
  qci-vision-frame-test
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/libobs"
)

target_link_libraries(
  qci-vision-frame-test
  PRIVATE
    "$<LINK_LIBRARY:FRAMEWORK,CoreVideo.framework>"
    "$<LINK_LIBRARY:FRAMEWORK,CoreFoundation.framework>"
)

add_test(NAME qci-vision-frame COMMAND qci-vision-frame-test)
set_tests_properties(qci-vision-frame PROPERTIES LABELS qci)

set_target_properties(qci-vision-frame-test PROPERTIES FOLDER plugins/qci-vision-person)
