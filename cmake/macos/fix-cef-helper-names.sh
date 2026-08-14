#!/bin/bash
# Rename the CEF helper bundles to track this fork's executable name.
#
# WHY THIS EXISTS
# ───────────────
# CEF does not ask us where its helper processes are. On macOS it derives the path from the
# basename of the RUNNING EXECUTABLE: <app>.app/Contents/Frameworks/<exe> Helper (GPU).app/...
# plugins/obs-browser hardcodes `set(helper_output_name "OBS Helper")`, which is correct upstream
# because upstream's executable is named `OBS`. This fork renames the executable to `QCi-Studio`
# (cmake/macos/helpers.cmake), so CEF looked for "QCi-Studio Helper (GPU).app", found nothing,
# and failed to launch the GPU process. Chromium retries nine times and then calls FATAL:
#
#   ERROR:gpu_process_host.cc(1001)  GPU process launch failed: error_code=1003
#   WARNING:gpu_process_host.cc(1443) The GPU process has crashed 9 time(s)
#   FATAL:gpu_data_manager_impl_private.cc(454) GPU process isn't usable. Goodbye.
#
# FATAL takes the WHOLE APP down, a few seconds after startup, every launch. That is the crash
# that made the fork look unusable. Measured 2026-08-14: with the helpers renamed to match, the
# app runs past 25s with obs-browser loaded and logs no GPU errors at all.
#
# obs-browser is a SUBMODULE and must not be edited, so the rename happens here, after the Embed
# Frameworks phase has copied and signed the helpers into the bundle.
#
# Renaming invalidates each helper's signature (the bundle name, the inner binary name and
# CFBundleExecutable all change), so each one is re-signed afterwards. They are LEAF bundles with
# no nested code, so an ad-hoc signature is sufficient for local use; pass an identity as $2 to
# sign them for distribution.
set -euo pipefail

APP_BUNDLE="${1:?usage: fix-cef-helper-names.sh <path/to/App.app> [codesign-identity]}"
IDENTITY="${2:--}"
[[ -z "$IDENTITY" ]] && IDENTITY="-"

FRAMEWORKS="${APP_BUNDLE}/Contents/Frameworks"
[[ -d "$FRAMEWORKS" ]] || { echo "fix-cef-helper-names: no Frameworks dir in ${APP_BUNDLE}"; exit 0; }

# The name CEF will look for is the basename of the executable it is running inside.
EXE_NAME="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "${APP_BUNDLE}/Contents/Info.plist")"
[[ -n "$EXE_NAME" ]] || { echo "fix-cef-helper-names: could not read CFBundleExecutable"; exit 1; }

renamed=0
for suffix in "" " (GPU)" " (Plugin)" " (Renderer)"; do
  src="${FRAMEWORKS}/OBS Helper${suffix}.app"
  dst="${FRAMEWORKS}/${EXE_NAME} Helper${suffix}.app"

  # Already correct (upstream name == our name, or a previous build did the work).
  [[ "$src" == "$dst" ]] && continue
  [[ -d "$src" ]] || continue

  # COPY, never move. Xcode's Embed Frameworks phase tracks each helper at its original path and
  # fails the NEXT build with "Couldn't load Info dictionary for .../OBS Helper (GPU).app" if the
  # original is gone. The duplicates are thin launcher executables — the bulk of CEF lives in the
  # shared Chromium Embedded Framework.framework, which is not duplicated.
  rm -rf "$dst"
  /bin/cp -R "$src" "$dst"
  mv "$dst/Contents/MacOS/OBS Helper${suffix}" "$dst/Contents/MacOS/${EXE_NAME} Helper${suffix}"

  # CFBundleExecutable must match the renamed binary or the helper will not launch either.
  /usr/libexec/PlistBuddy -c "Set :CFBundleExecutable ${EXE_NAME} Helper${suffix}" "$dst/Contents/Info.plist"
  /usr/libexec/PlistBuddy -c "Set :CFBundleName ${EXE_NAME} Helper${suffix}" "$dst/Contents/Info.plist" 2>/dev/null || true

  codesign --force --sign "$IDENTITY" "$dst" >/dev/null 2>&1
  renamed=$((renamed + 1))
done

echo "fix-cef-helper-names: ${renamed} CEF helper bundle(s) now named '${EXE_NAME} Helper*'"
