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
# enable_testing() rather than include(CTest): CTest pulls in Dart/CDash submission targets this
# fork will never use. `ctest --test-dir build_macos` is the entry point.
enable_testing()
# find_program, not a bare `python3`: ctest does not resolve the command through the caller's PATH,
# so `COMMAND python3 …` reports the test as "Not Run" — which ctest counts as a FAILURE but reads
# in the summary like an environment quirk rather than a broken check. A test that cannot start is
# the one shape of red that gets ignored.
find_program(QCI_PYTHON NAMES python3 python REQUIRED)
add_test(NAME qci-branding COMMAND "${QCI_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/build-aux/check-branding.py")
set_tests_properties(qci-branding PROPERTIES LABELS qci)

message_configuration()
