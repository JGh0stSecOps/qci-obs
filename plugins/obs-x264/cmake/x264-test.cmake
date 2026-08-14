add_executable(obs-x264-test)

target_sources(obs-x264-test PRIVATE obs-x264-test.c)

target_compile_options(obs-x264-test PRIVATE $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang>:-Wno-strict-prototypes>)

target_link_libraries(obs-x264-test PRIVATE OBS::opts-parser)

# ── QCi: THIS TEST LINKS libobs WHETHER IT WANTS TO OR NOT ──────────────────────────────────────
# opts-parser is an OBJECT library that links OBS::libobs PUBLIC (shared/opts-parser/CMakeLists.txt),
# so this executable carries a load command for @rpath/libobs.framework/Versions/A/libobs even
# though obs-x264-test.c never calls a libobs symbol.
#
# On macOS the only rpath it was given is CMAKE_INSTALL_RPATH's "@executable_path/../Frameworks"
# (cmake/macos/defaults.cmake:38), which resolves to plugins/obs-x264/Frameworks — a directory that
# exists in an installed .app and never in the build tree. dyld then aborts BEFORE main() runs, so
# ctest reported "Subprocess aborted" and not a test failure: the binary never got far enough to
# have an opinion about opts-parser. That is the worst shape of red, because it reads like an
# environment quirk rather than a broken check — the same trap the find_program(QCI_PYTHON) note in
# the root CMakeLists calls out.
#
# It went unnoticed because until enable_testing() was moved above add_subdirectory() this test was
# never registered at all (see the root CMakeLists comment). Fixing the registration is what made
# the breakage visible.
#
# The rpath must name the directory CONTAINING libobs.framework, not the bundle itself, and it must
# resolve per-configuration because this is a multi-config (Xcode) build — hence
# "$<TARGET_BUNDLE_DIR:OBS::libobs>/.." rather than a hardcoded path.
#
# ⚠️ ON macOS, INSTALL_RPATH AND BUILD_RPATH ARE BOTH INERT IN THIS TREE. cmake/macos/xcode.cmake:165
# sets CMAKE_XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS globally, and an explicit Xcode attribute wins
# over CMake's own rpath translation — so every target in the generated project gets that one runpath
# and nothing else, no matter what the CMake rpath properties say.
#
# This is worth knowing before debugging it a second time: setting INSTALL_RPATH or BUILD_RPATH here
# configures with no warning, builds green, and changes nothing. Both were tried and both were
# confirmed dead by reading LD_RUNPATH_SEARCH_PATHS back out of the generated .xcodeproj
# (`grep -o 'LD_RUNPATH_SEARCH_PATHS = [^;]*;' build_macos/obs-studio.xcodeproj/project.pbxproj`)
# and by `otool -l` on the relinked binary. The target-level XCODE_ATTRIBUTE_* below is what actually
# overrides the global one. The non-Xcode branch keeps BUILD_RPATH, which IS honoured by the
# single-config generators used on Linux and Windows.
#
# Xcode's LD_RUNPATH_SEARCH_PATHS is SPACE-separated (CMake's rpath properties are ;-separated), and
# the default is kept as the first entry so this target's runpath is a superset of what it had.
#
# ⚠️ AND THE RPATH ALONE IS NOT ENOUGH — THE SIGNATURES HAVE TO AGREE TOO. With only the runpath
# fixed, dyld finds the framework and then still refuses it:
#
#   code signature ... not valid for use in process: mapping process and mapped file
#   (non-platform) have different Team IDs
#
# because libobs.framework is adhoc/linker-signed with no TeamIdentifier (verified with
# `codesign -dv`), while this test — a plain add_executable() that never goes through
# set_target_properties_obs() — inherits the project-wide Manual signing from
# cmake/macos/xcode.cmake:23-35 and the hardened runtime from lines 49-51. Library validation is
# part of the hardened runtime, and it will not map an adhoc library into a team-signed process.
#
# So the test is signed adhoc, exactly like the framework it loads. That is the right shape for a
# binary that is never installed into the .app and never shipped, and it is the same treatment
# obs-ffmpeg-mux already gets in cmake/macos/helpers.cmake:366-368. Note what is deliberately NOT
# done here: adding com.apple.security.cs.disable-library-validation. That entitlement would also
# make the load succeed, and it would do so by weakening a security control on a target that has no
# need of one — the fix is to stop over-signing the test, not to punch a hole for it.
#
# ⚠️ AND libobs DRAGS THE FFMPEG DYLIBS IN BEHIND IT. libobs links @rpath/libavcodec.dylib,
# libavformat, libavutil, libswresample and libswscale (`otool -L` on the framework binary). The
# .app never notices because _bundle_dependencies() copies them into Contents/Frameworks, which is
# what "@executable_path/../Frameworks" is for — but a test executable run straight out of the build
# tree has no bundle, so the deps prefix has to be on the runpath explicitly.
#
# find_library against CMAKE_PREFIX_PATH with NO_DEFAULT_PATH, rather than a hardcoded .deps path or
# a bare find: the prefix is versioned (.deps/obs-deps-2026-07-15-universal) so hardcoding rots at
# the next deps bump, and an unscoped find_library would happily bind to /opt/homebrew/lib, which
# also ships a libavcodec.dylib of the same soname. Loading a DIFFERENT ffmpeg than the one libobs
# was linked against is precisely the sort of thing that "works" until it segfaults somewhere else.
find_library(
  QCI_X264_TEST_AVCODEC
  NAMES avcodec
  PATHS ${CMAKE_PREFIX_PATH}
  PATH_SUFFIXES lib
  NO_DEFAULT_PATH
  REQUIRED
)
get_filename_component(QCI_X264_TEST_DEPS_LIB "${QCI_X264_TEST_AVCODEC}" DIRECTORY)
mark_as_advanced(QCI_X264_TEST_AVCODEC)

if(OS_MACOS)
  set_target_properties(
    obs-x264-test
    PROPERTIES XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS
               "@executable_path/../Frameworks $<TARGET_BUNDLE_DIR:OBS::libobs>/.. ${QCI_X264_TEST_DEPS_LIB}"
               XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME NO
               XCODE_ATTRIBUTE_CODE_SIGN_STYLE Manual
               XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-"
               XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ""
  )
else()
  set_target_properties(obs-x264-test PROPERTIES BUILD_RPATH "$<TARGET_FILE_DIR:OBS::libobs>")
endif()

add_test(NAME obs-x264-test COMMAND obs-x264-test)

set_target_properties(obs-x264-test PROPERTIES FOLDER plugins/obs-x264)
