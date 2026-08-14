cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(obs-studio VERSION ${OBS_VERSION_CANONICAL})

if(CMAKE_HOST_SYSTEM_NAME MATCHES "Windows")
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows/architecture.cmake")
  if(NOT OBS_PARENT_ARCHITECTURE STREQUAL CMAKE_VS_PLATFORM_NAME)
    return()
  endif()
endif()

include(compilerconfig)
include(defaults)
include(helpers)

option(ENABLE_FRONTEND "Enable building with UI (requires Qt)" ON)
option(ENABLE_SCRIPTING "Enable scripting support" ON)
option(ENABLE_HEVC "Enable HEVC encoders" ON)

# ── QCi: enable_testing() MUST come before every add_subdirectory() ─────────────────────────────
# It used to sit at the bottom of this file, next to the qci-branding test it was added for. That
# placement silently threw away every add_test() in every subdirectory.
#
# CMake enables testing per directory, and a subdirectory inherits the setting as it stood when
# add_subdirectory() ran. With the call below the plugins/ line, `add_test()` inside a plugin
# generated NO CTestTestfile entry — no warning, no error, and `ctest -N` simply did not list it.
# That is not hypothetical: plugins/obs-x264/cmake/x264-test.cmake has called add_test() this whole
# time and obs-x264-test has never once run in this tree.
#
# Reproduced before moving it, with two throwaway projects differing only in this ordering:
#   enable_testing() after  add_subdirectory  ->  ctest -N lists 1 test (the root one)
#   enable_testing() before add_subdirectory  ->  ctest -N lists 2 tests (root + subdirectory)
#
# enable_testing() rather than include(CTest): CTest pulls in Dart/CDash submission targets this
# fork will never use. `ctest --test-dir build_macos` is the entry point.
enable_testing()

add_subdirectory(libobs)
if(OS_WINDOWS)
  add_subdirectory(libobs-d3d11)
  add_subdirectory(libobs-winrt)
endif()
add_subdirectory(libobs-opengl)
if(OS_MACOS)
  add_subdirectory(libobs-metal)
endif()
add_subdirectory(plugins)

add_subdirectory(test/test-input)

add_subdirectory(frontend)

# ── QCi: checks that run under ctest ────────────────────────────────────────────────────────────
# Upstream ships no test wiring at all — no enable_testing(), no add_test() anywhere in the tree —
# which is why the rebrand's verify phase had nothing to run and 146 translated strings shipped
# claiming this fork uploads crash reports to the OBSProject. That claim was true in en-US, which
# had been rewritten, and false in every other language, which had not.
#
# enable_testing() itself now lives above add_subdirectory(libobs) — see the comment there for why
# its original position here quietly discarded every subdirectory test.
#
# find_program, not a bare `python3`: ctest does not resolve the command through the caller's PATH,
# so `COMMAND python3 …` reports the test as "Not Run" — which ctest counts as a FAILURE but reads
# in the summary like an environment quirk rather than a broken check. A test that cannot start is
# the one shape of red that gets ignored.
find_program(QCI_PYTHON NAMES python3 python REQUIRED)
add_test(NAME qci-branding COMMAND "${QCI_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/build-aux/check-branding.py")
set_tests_properties(qci-branding PROPERTIES LABELS qci)

message_configuration()
