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
LOCALE_DIR = ROOT / "frontend" / "data" / "locale"

# Keys whose values legitimately name upstream, because they describe importing from it.
ALLOWED_KEYS = {"OBSStudio", "OBSClassic"}

# The product noun, as a whole word. "OBS-i", "OBS-ն" etc. match too: the noun is what is wrong,
# and the surrounding inflection is the translator's, not ours.
BRANDING_RE = re.compile(r"\bOBS\b")

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


def en_us_values():
    """key -> en-US value. The reference every other locale is judged against."""
    return {k: v for _n, _raw, k, v in parse(LOCALE_DIR / "en-US.ini")}


def parse(path):
    """Yield (lineno, raw_line, key, value) for every key=value line."""
    for n, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        s = raw.lstrip()
        if not s or s.startswith("#") or "=" not in raw:
            continue
        key, _, val = raw.partition("=")
        yield n, raw, key.strip(), val


def is_stale_upstream_claim(key, val, en):
    """True when this value refers to upstream but the corrected en-US value for the same key does
    not. Presence alone is fine — see UPSTREAM_RE's comment."""
    if not UPSTREAM_RE.search(val):
        return False
    return not UPSTREAM_RE.search(en.get(key, ""))


def scan():
    en = en_us_values()
    branding, stale = [], []
    for path in sorted(LOCALE_DIR.glob("*.ini")):
        for n, _raw, key, val in parse(path):
            if key in ALLOWED_KEYS:
                continue
            if BRANDING_RE.search(val):
                branding.append((path, n, key, val.strip()[:100]))
            if path.name != "en-US.ini" and is_stale_upstream_claim(key, val, en):
                stale.append((path, n, key, val.strip()[:100]))
    return branding, stale


def fix():
    """Substitute the product noun; DELETE keys whose translation makes a claim en-US has retracted."""
    en = en_us_values()
    subbed = deleted = 0
    for path in sorted(LOCALE_DIR.glob("*.ini")):
        if path.name == "en-US.ini":
            continue  # the source of truth, and already correct
        out, changed = [], False
        for raw in path.read_text(encoding="utf-8").splitlines(keepends=True):
            key = raw.partition("=")[0].strip() if "=" in raw else ""
            val = raw.partition("=")[2] if "=" in raw else ""
            if key and key not in ALLOWED_KEYS and is_stale_upstream_claim(key, val, en):
                # Dropped, not rewritten: it falls back to the corrected en-US. Docstring says why.
                deleted += 1
                changed = True
                continue
            if key and key not in ALLOWED_KEYS and BRANDING_RE.search(val):
                head, sep, tail = raw.partition("=")
                raw = head + sep + BRANDING_RE.sub("QCi Studio", tail)
                subbed += 1
                changed = True
            out.append(raw)
        if changed:
            path.write_text("".join(out), encoding="utf-8")
    return subbed, deleted


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fix", action="store_true")
    args = ap.parse_args()

    if args.fix:
        subbed, deleted = fix()
        print(f"substituted {subbed} product nouns; deleted {deleted} stale keys "
              f"(they now fall back to en-US)")

    branding, stale = scan()

    if stale:
        print(f"\n✖ {len(stale)} translated value(s) still claim something en-US has retracted.")
        print("  The en-US value for these keys no longer refers to upstream; the translation does.")
        print("  Delete the key so it falls back to en-US — do not translate a replacement.\n")
        for path, n, k, v in stale[:12]:
            print(f"    {path.name}:{n} {k}\n      {v}")
        if len(stale) > 12:
            print(f"    … and {len(stale) - 12} more")

    if branding:
        print(f"\n✖ {len(branding)} value(s) still call this product OBS.\n")
        for path, n, k, v in branding[:12]:
            print(f"    {path.name}:{n} {k}\n      {v}")
        if len(branding) > 12:
            print(f"    … and {len(branding) - 12} more")

    if branding or stale:
        print("\nRe-run with --fix to apply the mechanical repair.")
        return 1

    n_files = len(list(LOCALE_DIR.glob("*.ini")))
    # A pass that checked nothing is not a pass — the same rule the rig's contract tests use.
    if n_files < 50:
        print(f"✖ only {n_files} locale files found — the scan is looking in the wrong place")
        return 1
    print(f"✓ {n_files} locale files: no OBS branding in values, nothing claims upstream's services")
    return 0


if __name__ == "__main__":
    sys.exit(main())
