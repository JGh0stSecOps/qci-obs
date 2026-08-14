#!/bin/zsh
# ---------------------------------------------------------------------------------------------
# install-qci-plugins.command — put the two THIRD-PARTY privacy-chain plugins where QCi Studio
# can find them.
#
#   obs-shaderfilter.plugin       filter id "shader_filter"
#   obs-backgroundremoval.plugin  filter id "background_removal"
#
# ⚠️ TWO DIFFERENT COLLECTIONS, TWO DIFFERENT COUNTS — DO NOT "CORRECT" ONE AGAINST THE OTHER.
# Measured 2026-08-14 by walking the "filters" arrays of the FORK's collection,
# ~/Library/Application Support/qci-studio/basic/scenes/Untitled.json (67 sources):
#     shader_filter 8, background_removal 2, vision_person_mask 2
#   (plus gpu_delay 9, color_filter 3, compressor_filter 2, scale_filter 2, noise_gate_filter 1,
#    limiter_filter 1 — all libobs built-ins, nothing to install for those.)
#
# This header used to say "18 instances in the live collection", and vision-person-mask.mm still
# says 4 vision_person_mask instances. Those numbers are NOT necessarily wrong: "the live
# collection" is the STOCK OBS one, which is still driving the real stream and which this fork's
# tooling is forbidden to read. The fork's copy is a separate, and evidently smaller, file. So the
# fork-side counts above are stated as fork-side counts, and the stock-side numbers are left alone
# rather than being overwritten with numbers measured from the wrong file. If you ever need the
# stock-side figure, get it from the operator — do not go read that tree to settle it.
#
# It deliberately does NOT install obs-vision-person. That one is now FIRST-PARTY: it is built
# from plugins/qci-vision-person/ and embedded in the app at Contents/PlugIns/, which libobs adds
# as a module path in obs-cocoa.m:add_default_module_paths(). Copying the old standalone bundle in
# as well would register a second source with the same id, "vision_person_mask" — libobs keeps the
# first and logs the second as a duplicate, so which mask the operator ends up tuning would depend
# on module load order. The check at the end refuses to leave that landmine lying around.
#
# ---------------------------------------------------------------------------------------------
# WHY THIS SCRIPT HAS TO EXIST AT ALL — i.e. why the fork does not simply inherit stock OBS's
# plugins, which would need no script:
#
# QCi Studio searches ~/Library/Application Support/qci-studio/plugins/, built from
# OBS_USER_DATA_DIR, and never ~/Library/Application Support/obs-studio/plugins/. That is a
# decision, not an oversight, and the reasoning is written down at
# frontend/widgets/QCiBasic.cpp:145-155:
#
#     "loading another installation's plugins means running its binaries in this process, and
#      those plugins then write their own settings back into this build's plugin_config. The fork
#      already corrupted a live rig's global.ini by sharing the directory name; sharing the plugin
#      tree is the same mistake with arbitrary code attached."
#
# The operator's stock OBS is the machine that runs a live stream. A shared plugin tree means one
# obs-shaderfilter.plugin loaded into two processes writing two plugin_configs, and it means an
# experimental fork build executing whatever binaries the production install happens to contain.
# So the fork keeps its own directory, and the cost of that is this script: anything the rig needs
# has to be COPIED in, once, deliberately.
#
# Copies only. Nothing here writes to, moves, or deletes anything under the stock obs-studio
# directory; that tree is read-only to this script, and the live rig keeps running off it.
# ---------------------------------------------------------------------------------------------

set -u
set -o pipefail

SRC="$HOME/Library/Application Support/obs-studio/plugins"
DST="$HOME/Library/Application Support/qci-studio/plugins"

# The two third-party bundles this fork needs. obs-vision-person is NOT here, on purpose.
WANTED=(obs-shaderfilter obs-backgroundremoval)

# Built in-tree instead of copied — used only to detect a stale copy in the destination.
FIRST_PARTY=(obs-vision-person)

ok()   { print -r -- "  ok    $*" }
info() { print -r -- "        $*" }
warn() { print -r -- "  WARN  $*" }
die()  { print -r -- "" ; print -r -- "  FAIL  $*" ; print -r -- "" ; exit 1 }

# Read a key out of a bundle's Info.plist. Prints the value, or nothing at all if the key or the
# plist is absent. Absence is NOT an error here: obs-backgroundremoval.plugin genuinely ships with
# no CFBundleIdentifier (measured), and it loads in stock OBS every day regardless, because libobs
# finds modules by path — <name>.plugin/Contents/MacOS/<name> — and never consults the identifier.
plist_get() {
  local bundle="$1" key="$2"
  /usr/libexec/PlistBuddy -c "Print :$key" "$bundle/Contents/Info.plist" 2>/dev/null
}

print -r -- ""
print -r -- "QCi Studio — install third-party privacy-chain plugins"
print -r -- "  from: $SRC"
print -r -- "  to:   $DST"
print -r -- ""

# ── refuse before touching anything ──────────────────────────────────────────────────────────
[[ -d "$SRC" ]] || die "source directory does not exist: $SRC"

typeset -a missing
for name in $WANTED; do
  bundle="$SRC/$name.plugin"
  [[ -d "$bundle" ]]                            || { missing+=("$name.plugin (no such bundle)"); continue }
  [[ -f "$bundle/Contents/MacOS/$name" ]]       || { missing+=("$name.plugin (no Contents/MacOS/$name)"); continue }
done

if (( ${#missing} )); then
  print -r -- "  Refusing to install. Missing in $SRC:"
  for m in $missing; do print -r -- "    - $m"; done
  die "install nothing rather than half a privacy chain (${#missing} of ${#WANTED} unavailable)"
fi

mkdir -p "$DST" || die "could not create $DST"

# ── copy ─────────────────────────────────────────────────────────────────────────────────────
# ditto, not cp -R: it is the macOS tool that copies a bundle whole — extended attributes,
# resource forks and the embedded code signature included — and it is unaffected by the operator's
# `cp` -> `cp -i` alias.
typeset -a installed
for name in $WANTED; do
  src="$SRC/$name.plugin"
  dst="$DST/$name.plugin"

  if [[ -e "$dst" ]]; then
    info "replacing existing $name.plugin"
    rm -rf "$dst" || die "could not remove the old $dst"
  fi

  ditto "$src" "$dst" || die "ditto failed copying $name.plugin"
  installed+=("$name")
done

# ── verify what actually landed ──────────────────────────────────────────────────────────────
print -r -- ""
print -r -- "Verifying:"
typeset -i problems=0

for name in $installed; do
  src="$SRC/$name.plugin"
  dst="$DST/$name.plugin"

  # 1. CFBundleIdentifier — compare destination against source rather than demanding one exists.
  #    A mismatch means the copy is not the bundle we were asked to copy; equal-and-absent is a
  #    real, expected state for obs-backgroundremoval.
  src_id="$(plist_get "$src" CFBundleIdentifier)"
  dst_id="$(plist_get "$dst" CFBundleIdentifier)"
  if [[ "$src_id" != "$dst_id" ]]; then
    warn "$name.plugin: CFBundleIdentifier changed in the copy — source '${src_id:-<none>}', copy '${dst_id:-<none>}'"
    problems=$(( problems + 1 ))
  elif [[ -z "$dst_id" ]]; then
    ok "$name.plugin  CFBundleIdentifier: <none> (matches source; libobs loads by path, not by id)"
  else
    ok "$name.plugin  CFBundleIdentifier: $dst_id"
  fi

  # 2. The file libobs will actually dlopen.
  if [[ -f "$dst/Contents/MacOS/$name" ]]; then
    info "loadable at $name.plugin/Contents/MacOS/$name"
  else
    warn "$name.plugin: Contents/MacOS/$name is missing — QCi Studio will not load this module"
    problems=$(( problems + 1 ))
  fi

  # 3. Architecture. A plugin without the host's slice fails to load with nothing in the UI to
  #    explain it, so say it out loud now rather than leaving it to a silent no-show later.
  archs="$(lipo -archs "$dst/Contents/MacOS/$name" 2>/dev/null)"
  host="$(uname -m)"
  if [[ -n "$archs" && "$archs" != *"$host"* ]]; then
    warn "$name.plugin: built for '$archs', this Mac is '$host' — it will not load"
    problems=$(( problems + 1 ))
  else
    info "arch: ${archs:-unknown} (host $host)"
  fi
done

# ── the one bundle that must NOT be here ─────────────────────────────────────────────────────
for name in $FIRST_PARTY; do
  if [[ -e "$DST/$name.plugin" ]]; then
    print -r -- ""
    warn "$name.plugin is present in $DST"
    warn "It is built into the app now (plugins/qci-vision-person). Two modules registering"
    warn "'vision_person_mask' means load order decides which one the operator is tuning."
    warn "Remove it by hand once you have confirmed the built-in mask works:"
    warn "    rm -rf \"$DST/$name.plugin\""
    problems=$(( problems + 1 ))
  fi
done

# ── summary ──────────────────────────────────────────────────────────────────────────────────
print -r -- ""
print -r -- "Installed into $DST:"
for name in $installed; do print -r -- "  - $name.plugin"; done
print -r -- ""
print -r -- "Not installed, by design:"
print -r -- "  - obs-vision-person.plugin — first-party now, built from plugins/qci-vision-person"
print -r -- "    and embedded at QCi-Studio.app/Contents/PlugIns/"
print -r -- ""
print -r -- "Stock OBS was not modified. Its plugins are still at:"
print -r -- "  $SRC"
print -r -- ""

if (( problems )); then
  print -r -- "Finished with $problems thing(s) to look at above."
  exit 2
fi

print -r -- "All checks passed. Start QCi Studio and confirm in its log that it loaded:"
print -r -- "  obs-shaderfilter, obs-backgroundremoval, qci-vision-person"
print -r -- ""
exit 0
