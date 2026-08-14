#!/usr/bin/env python3
"""Fail the build if this fork still calls itself OBS, or still describes upstream's services.

WHY THIS EXISTS. The fork rename touched 324 files and the phase that was supposed to verify it
never ran. What survived was not cosmetic:

  * 28 of 77 locale files still said "OBS" in translated user-visible values while en-US was clean.
  * 47 locale files still carried CrashHandling.Labels.PrivacyNotice promising the crash report is
    uploaded automatically to the OBSProject, with a link to obsproject.com/privacy-policy.
    THIS FORK HAS CRASH UPLOAD DISABLED. en-US had been rewritten to say what actually happens; the
    translations had not, so the string was factually FALSE in ~47 languages while being perfectly
    true in the one language anybody checked.

That is the failure this script is aimed at: a string that is right where you look and wrong
everywhere else.

TWO KINDS OF OFFENCE, AND THEY HAVE DIFFERENT FIXES.

  BRANDING  — "OBS" appearing as the product's name in a value. Fix by substitution.
  UNTRUE    — text describing an upstream service this fork does not use (crash upload, the update
              server, the privacy policy). Fix by DELETING the translated key, not by rewriting it.
              QCiApp.cpp:734-739 loads en-US first and overlays the user's locale on top, so a
              missing key falls back to en-US — which is the corrected text. Accurate English beats
              a confident lie in the reader's own language, and nobody here can review 47
              translations.

WHAT IS SCANNED, AND WHY IT IS EVERY LOCALE DIRECTORY THAT SHIPS.

The first version of this file scanned frontend/data/locale and stopped there — 77 of the 2274 .ini
files in the tree, 3% — and printed an unscoped green "no OBS branding in values" while
plugins/mac-capture/data/locale/en-US.ini:17 still read DisplayCapture.HideOBS="Hide OBS from
capture", an en-US string sitting in the Screen Capture source's property sheet. libobs loads every
module's data/locale/*.ini at module load, so a plugin string and a frontend string land in the same
window; nothing about the widget tells the reader which subtree it came from. A checker with that
blind spot does not merely miss offences — it reproduces the incident described above, one storey up.

So the scan walks every shipping locale directory (37 of them after exclusions) and judges each one
against ITS OWN en-US.ini. There is deliberately no global reference: every module owns its key
namespace, and comparing e.g. obs-filters' de-DE against the frontend's en-US would find no key at
all and read every value as a divergence.

WHAT IS DELIBERATELY ALLOWED.

  * KEYS are never scanned — only values. Renaming a key silently yields an empty string in the UI,
    which reads as a missing label rather than a bug, so keys are left strictly alone.
  * OBSStudio / OBSClassic are importer labels: they name the thing being imported FROM. Correct.
  * `Remux.OBSRecording`, `HideOBSWindowsFromCapture` and friends are key names, covered above.

Run: python3 build-aux/check-branding.py [--fix]
Exit 0 clean, 1 with offences.
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Where the shipping locale files are. Rooted at the two source trees rather than a bare
# ROOT.glob("**/data/locale/*.ini"): the latter also sweeps build_macos/ and .deps/, where the same
# files exist as stale staged copies, and a checker that reports on build output reports on whatever
# was true the last time somebody built.
LOCALE_GLOBS = ("frontend/data/locale/*.ini", "plugins/**/data/locale/*.ini")

# Locale directories NOT scanned, each with the reason it cannot be fixed here. Their offences are
# still counted and printed below: an exclusion that vanishes from the output is indistinguishable
# from a clean tree, which is the failure mode this whole file is about.
EXCLUDED_DIRS = {
    # Both are git submodules (see .gitmodules) pinned to upstream's own repositories. Their "OBS"
    # strings — obs-browser's "Full access to OBS (Start/Stop streaming without warning, etc.)",
    # obs-websocket's "Remote-control of OBS Studio through WebSocket" — really are wrong in this
    # fork's UI, but the edit is not this fork's to make: it lives outside this repository's history
    # and the next submodule update throws it away. Fixing them means a patch upstream or a locale
    # override carried by the frontend, not a rewrite of a checked-out submodule.
    "plugins/obs-browser/data/locale",
    "plugins/obs-websocket/data/locale",
}

# Exempt from the per-directory "you parsed nothing" floor further down. plugins/mac-virtualcam's
# top-level locale directory holds exactly one file and it is ZERO BYTES: that module's real strings
# live in plugins/mac-virtualcam/src/obs-plugin/data/locale, which IS scanned. An empty placeholder
# is a legitimate shape here (qci-vision-person ships one for the same reason — obs_module_load_locale
# logs a failure for every module that has no en-US.ini at all), so it is named rather than silently
# tolerated by a floor of zero.
EMPTY_BY_DESIGN = {"plugins/mac-virtualcam/data/locale"}

# Keys whose values legitimately name upstream, because they describe importing from it.
ALLOWED_KEYS = {"OBSStudio", "OBSClassic"}

# The product noun, as a whole word. "OBS-i", "OBS-ն" etc. match too: the noun is what is wrong,
# and the surrounding inflection is the translator's, not ours.
BRANDING_RE = re.compile(r"\bOBS\b")

# What --fix writes, and what it consumes. The optional " Studio" is load-bearing: substituting the
# bare noun into "Please allow OBS Studio to install…" (pl-PL and three others) yields "QCi Studio
# Studio". The detection regex above stays the bare noun on purpose — the offence is the noun.
PRODUCT = "QCi Studio"
BRANDING_SUB_RE = re.compile(r"\bOBS(?:[  -]?Studio)?\b")

# Text referring to upstream's site or organisation.
#
# ⚠️ PRESENCE ALONE IS NOT AN OFFENCE, and a first draft of this script got that wrong. It flagged
# every value containing obsproject.com — including `Basic.Settings.Stream.WHIPSimulcastInfo`, whose
# en-US value STILL links https://obsproject.com/kb/whip-streaming-guide because that is a genuine
# technical article about the WHIP protocol, not a claim about this fork's services. Deleting those
# 77 translations would have destroyed real information to fix a problem they did not have.
#
# The offence is DIVERGENCE: a translated value referring to upstream for a key where the en-US
# value no longer does. That is precisely the incident — en-US was rewritten to tell the truth and
# the translations kept the old claim — and it is decidable per key rather than by taste.
UPSTREAM_RE = re.compile(r"obsproject\.com|OBSProject", re.IGNORECASE)

# libobs' own parser treats only '#' as a comment (libobs/util/config-file.c:180), but this fork's
# locale files (plugins/qci-*/data/locale/en-US.ini) carry ';'-prefixed header blocks. Both are
# skipped here: a ';' line is not a value, and counting it as unparseable would make the corruption
# guard below cry wolf on the two files this fork wrote itself. (No ';' line in the tree contains an
# '=', so nothing that libobs would read as a key is being hidden by this.)
COMMENT_PREFIXES = ("#", ";")

# Anti-vacuity floors. The previous guard re-globbed one directory and counted FILES — so a corpus
# that stopped PARSING still counted 77 of them and printed the all-clear. Emptying every translated
# file, or switching the separator from '=' to ': ', both scanned zero values, found zero offences
# and exited 0. What has to be non-trivial is the number of key/value pairs actually EXTRACTED.
#
# Measured on this tree, after the exclusions above: 37 directories, 2149 files, 125,154 values.
# The floors sit well below that deliberately. They exist to catch a corpus that COLLAPSED, not one
# that shrank: this fork already dropped the AJA and fdk-aac plugins, and a checker that goes red
# when a plugin is deleted gets edited until it stops going red.
MIN_DIRS = 25
MIN_FILES = 1200
MIN_VALUES = 60000


def rel(path):
    """Repo-relative path, POSIX-shaped — 37 directories all ship a `de-DE.ini`, so a bare
    path.name in a failure message no longer identifies anything."""
    return path.relative_to(ROOT).as_posix()


def locale_dirs():
    """Every shipping locale directory, deduplicated and sorted."""
    dirs = set()
    for pattern in LOCALE_GLOBS:
        for ini in ROOT.glob(pattern):
            dirs.add(ini.parent)
    return sorted(dirs)


def en_us_values(locale_dir):
    """key -> en-US value for ONE directory. The reference its siblings are judged against."""
    return {k: v for _n, _raw, k, v in parse(locale_dir / "en-US.ini")}


def parse(path, unparsed=None):
    """Yield (lineno, raw_line, key, value) for every key=value line.

    A line that carries content but has no '=' is not a value — and dropping it silently is exactly
    how the whole corpus can stop parsing without anything going red. When `unparsed` is a list,
    those lines are recorded into it so main() can fail on them instead of reporting a clean scan of
    nothing.
    """
    for n, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        s = raw.lstrip()
        if not s or s.startswith(COMMENT_PREFIXES):
            continue
        if "=" not in raw:
            if unparsed is not None:
                unparsed.append((path, n, raw.strip()[:100]))
            continue
        key, _, val = raw.partition("=")
        yield n, raw, key.strip(), val


def is_stale_upstream_claim(key, val, en):
    """True when this value refers to upstream but the corrected en-US value for the same key does
    not. Presence alone is fine — see UPSTREAM_RE's comment."""
    if not UPSTREAM_RE.search(val):
        return False
    return not UPSTREAM_RE.search(en.get(key, ""))


def scan(dirs):
    """Scan every directory against its own en-US.ini.

    Returns (branding, stale, unparsed, per_dir) where per_dir is [(dir, n_files, n_values)] — the
    counts the vacuity guard is derived from, measured during the scan rather than re-globbed
    afterwards, so they describe what was actually read.
    """
    branding, stale, unparsed = [], [], []
    per_dir = []
    for d in dirs:
        # unparsed=None here on purpose: en-US.ini is read twice (once as the baseline, once as a
        # member of the directory) and its odd lines should be reported once.
        en = en_us_values(d)
        n_files = n_values = 0
        for path in sorted(d.glob("*.ini")):
            n_files += 1
            for n, _raw, key, val in parse(path, unparsed):
                n_values += 1
                if key in ALLOWED_KEYS:
                    continue
                if BRANDING_RE.search(val):
                    branding.append((path, n, key, val.strip()[:100]))
                if path.name != "en-US.ini" and is_stale_upstream_claim(key, val, en):
                    stale.append((path, n, key, val.strip()[:100]))
        per_dir.append((d, n_files, n_values))
    return branding, stale, unparsed, per_dir


def fix(dirs):
    """Substitute the product noun; DELETE keys whose translation makes a claim en-US has retracted."""
    subbed = deleted = 0
    for d in dirs:
        if not (d / "en-US.ini").exists():
            continue  # no baseline to judge against; main() fails on it loudly right after this
        en = en_us_values(d)
        for path in sorted(d.glob("*.ini")):
            # en-US is the reference for the stale check, so nothing is ever deleted from it — but
            # it is NOT exempt from branding. The earlier version skipped en-US entirely, which was
            # true of the frontend (already rewritten) and false of every plugin: 27 plugin en-US
            # values still named OBS, mac-capture's "Hide OBS from capture" among them.
            is_reference = path.name == "en-US.ini"
            out, changed = [], False
            for raw in path.read_text(encoding="utf-8").splitlines(keepends=True):
                key = raw.partition("=")[0].strip() if "=" in raw else ""
                val = raw.partition("=")[2] if "=" in raw else ""
                if key.startswith(COMMENT_PREFIXES):
                    key = ""  # a comment that happens to contain '='; not a key
                if key and key not in ALLOWED_KEYS and not is_reference and is_stale_upstream_claim(key, val, en):
                    # Dropped, not rewritten: it falls back to the corrected en-US. Docstring says why.
                    deleted += 1
                    changed = True
                    continue
                if key and key not in ALLOWED_KEYS and BRANDING_RE.search(val):
                    head, sep, tail = raw.partition("=")
                    raw = head + sep + BRANDING_SUB_RE.sub(PRODUCT, tail)
                    subbed += 1
                    changed = True
                out.append(raw)
            if changed:
                path.write_text("".join(out), encoding="utf-8")
    return subbed, deleted


def report(label, offences, hint_lines):
    print(f"\n✖ {len(offences)} {label}")
    for line in hint_lines:
        print(line)
    print()
    for path, n, k, v in offences[:12]:
        print(f"    {rel(path)}:{n} {k}\n      {v}")
    if len(offences) > 12:
        print(f"    … and {len(offences) - 12} more")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fix", action="store_true")
    args = ap.parse_args()

    dirs = locale_dirs()
    scanned = [d for d in dirs if rel(d) not in EXCLUDED_DIRS]
    excluded = [d for d in dirs if rel(d) in EXCLUDED_DIRS]

    if args.fix:
        subbed, deleted = fix(scanned)
        print(f"substituted {subbed} product nouns; deleted {deleted} stale keys "
              f"(they now fall back to en-US)")

    missing = [d for d in scanned if not (d / "en-US.ini").exists()]
    if missing:
        # Not a warning: en-US is the reference the divergence check is defined against, so a
        # directory without one is scanned but never actually judged. The old code hit this as a
        # FileNotFoundError traceback, which reads like a broken script rather than a finding.
        print(f"\n✖ {len(missing)} locale director(ies) have no en-US.ini to judge against:")
        for d in missing:
            print(f"    {rel(d)}")
        return 1

    branding, stale, unparsed, per_dir = scan(scanned)
    n_dirs = len(per_dir)
    n_files = sum(f for _d, f, _v in per_dir)
    n_values = sum(v for _d, _f, v in per_dir)

    # ── vacuity first, offences second ────────────────────────────────────────────────────────────
    # An empty offence list means one of two things and they are not the same: nothing is wrong, or
    # nothing was read. Deciding that BEFORE printing results is the whole point — a scan that did
    # not parse cannot be reported clean.
    if unparsed:
        print(f"\n✖ {len(unparsed)} line(s) carry content but no '=' — the corpus is not parsing as")
        print("  key=value, so an empty result below would mean nothing.\n")
        for path, n, raw in unparsed[:12]:
            print(f"    {rel(path)}:{n}\n      {raw}")
        if len(unparsed) > 12:
            print(f"    … and {len(unparsed) - 12} more")
        return 1

    starved = [(d, f, v) for d, f, v in per_dir if v == 0 and rel(d) not in EMPTY_BY_DESIGN]
    if starved:
        # Per-directory, not just in aggregate: one plugin's locale emptied out is ~2% of the total
        # value count and would be diluted into invisibility by a global floor.
        print(f"\n✖ {len(starved)} locale director(ies) yielded no key/value pairs at all:")
        for d, f, _v in starved:
            print(f"    {rel(d)} ({f} .ini file(s), 0 values)")
        return 1

    if n_dirs < MIN_DIRS or n_files < MIN_FILES or n_values < MIN_VALUES:
        print(f"✖ scanned {n_dirs} dirs / {n_files} files / {n_values} values — below the floor "
              f"({MIN_DIRS}/{MIN_FILES}/{MIN_VALUES}). A pass that checked nothing is not a pass.")
        return 1

    if stale:
        report("translated value(s) still claim something en-US has retracted.", stale, [
            "  The en-US value for these keys no longer refers to upstream; the translation does.",
            "  Delete the key so it falls back to en-US — do not translate a replacement.",
        ])

    if branding:
        report("value(s) still call this product OBS.", branding, [])

    if branding or stale:
        print("\nRe-run with --fix to apply the mechanical repair.")
        return 1

    print(f"✓ {n_dirs} locale dirs / {n_files} files / {n_values} values: no OBS branding in "
          f"values, nothing claims upstream's services")
    if excluded:
        # Printed on a GREEN run, every run. These offences are real and unfixed; they are just not
        # fixable in this repository (see EXCLUDED_DIRS). Reporting the count is what keeps the line
        # above from being read as "the app contains no OBS strings".
        #
        # Filtered on en-US.ini rather than assuming it: an excluded directory is a submodule
        # working copy, and this line is informational — it must not turn a clean tree red because
        # somebody's checkout is half-initialised.
        ex_dirs = [d for d in excluded if (d / "en-US.ini").exists()]
        ex_branding, ex_stale, _ignored, ex_per = scan(ex_dirs)
        ex_files = sum(f for _d, f, _v in ex_per)
        print(f"  ({len(ex_dirs)} dir(s) / {ex_files} files not scanned — {len(ex_branding)} branding "
              f"and {len(ex_stale)} stale offence(s) live there, see EXCLUDED_DIRS)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
