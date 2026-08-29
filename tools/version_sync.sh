#!/bin/sh
# tools/version_sync.sh - single-source version mechanism (R3-1).
#
# The root VERSION file is the ONLY authoritative version. Every other machine-readable version value
# is GENERATED or CHECKED from it, so there is exactly one place to edit and nothing can silently drift:
#   - include/keel/version.h         : freestanding-safe macros, committed + installed
#   - keel.pc.in Version field        : must be the @VERSION@ placeholder (no literal), substituted at
#                                       build time from VERSION by the Makefile
#   - sbom.cdx.json                   : metadata.component.version and .purl (pkg:github/...keel@<ver>)
#
# Modes:
#   --generate   write every derived value from VERSION (in-place)
#   --check      default-deny: fail if any derived value disagrees with VERSION (changes nothing)
#   --selftest   positive + negative canaries: parse a normal and a prerelease version, and prove the
#                check FAILS on an injected drift
#
# SemVer parse: MAJOR.MINOR.PATCH with an optional -PRERELEASE (e.g. 2.9.0 or 3.0.0-rc.1). The numeric
# KL_VERSION_NUMBER is MAJOR*10000 + MINOR*100 + PATCH (prerelease is NOT encoded numerically; it lives
# in KL_VERSION_STRING and KL_VERSION_PRERELEASE).
set -eu

ROOT=$(git rev-parse --show-toplevel 2>/dev/null || echo .)
cd "$ROOT"

VERSION_FILE=VERSION
VERSION_H=include/keel/version.h
PC_IN=keel.pc.in
SBOM=sbom.cdx.json

# parse_version <semver>: sets MAJOR MINOR PATCH PRERELEASE VNUM VSTR ISPRE. Returns 1 on a malformed value.
parse_version() {
    _v=$1
    VSTR=$_v
    _core=${_v%%-*}
    if [ "$_core" = "$_v" ]; then PRERELEASE=""; else PRERELEASE=${_v#*-}; fi
    case "$_core" in
        *.*.*) : ;;
        *) echo "version_sync: malformed version '$_v' (need MAJOR.MINOR.PATCH[-PRERELEASE])" >&2; return 1 ;;
    esac
    MAJOR=${_core%%.*}
    _rest=${_core#*.}
    MINOR=${_rest%%.*}
    PATCH=${_rest#*.}
    case "$MAJOR$MINOR$PATCH" in
        ''|*[!0-9]*) echo "version_sync: non-numeric core in '$_v'" >&2; return 1 ;;
    esac
    if [ -n "$PRERELEASE" ]; then ISPRE=1; else ISPRE=0; fi
    VNUM=$(( MAJOR * 10000 + MINOR * 100 + PATCH ))
}

# emit the canonical include/keel/version.h to stdout (from the parsed globals).
render_version_h() {
    printf '%s\n' '/*'
    printf '%s\n' ' * keel/version.h - Keel version macros. GENERATED from the root VERSION file by'
    printf '%s\n' ' * tools/version_sync.sh. DO NOT EDIT: change VERSION and run `make version-sync`.'
    printf '%s\n' ' * The check-version-drift gate fails if this file disagrees with VERSION.'
    printf '%s\n' ' *'
    printf '%s\n' ' * Freestanding-safe: macros only, no includes. Both umbrellas (keel.h, freestanding.h)'
    printf '%s\n' ' * include this. KL_VERSION_NUMBER is numeric (major*10000 + minor*100 + patch); a'
    printf '%s\n' ' * prerelease appears only in KL_VERSION_STRING and KL_VERSION_PRERELEASE.'
    printf '%s\n' ' */'
    printf '%s\n' '#ifndef KEEL_VERSION_H'
    printf '%s\n' '#define KEEL_VERSION_H'
    printf '\n'
    printf '#define KL_VERSION_MAJOR  %s\n' "$MAJOR"
    printf '#define KL_VERSION_MINOR  %s\n' "$MINOR"
    printf '#define KL_VERSION_PATCH  %s\n' "$PATCH"
    printf '#define KL_VERSION_STRING "%s"\n' "$VSTR"
    printf '#define KL_VERSION_PRERELEASE "%s"\n' "$PRERELEASE"
    printf '#define KL_VERSION_IS_PRERELEASE %s\n' "$ISPRE"
    printf '%s\n' '#define KL_VERSION_NUMBER \'
    printf '%s\n' '    ((KL_VERSION_MAJOR) * 10000 + (KL_VERSION_MINOR) * 100 + (KL_VERSION_PATCH))'
    printf '\n'
    printf '%s\n' '#endif /* KEEL_VERSION_H */'
}

# rewrite the keel component version + purl in the SBOM to $VSTR (metadata.component only: everything
# before the "components" dependency array). Reads stdin, writes stdout.
render_sbom() {
    awk -v ver="$VSTR" '
        /"components"[[:space:]]*:/ { indeps = 1 }
        indeps == 0 && vdone == 0 && /"version"[[:space:]]*:[[:space:]]*"[^"]*"/ {
            sub(/"version"[[:space:]]*:[[:space:]]*"[^"]*"/, "\"version\": \"" ver "\""); vdone = 1
        }
        indeps == 0 && /"purl"[[:space:]]*:[[:space:]]*"pkg:github\/artalis-io\/keel@[^"]*"/ {
            sub(/keel@[^"]*"/, "keel@" ver "\"")
        }
        { print }
    '
}

do_generate() {
    parse_version "$(cat "$VERSION_FILE")"
    render_version_h > "$VERSION_H.tmp" && mv "$VERSION_H.tmp" "$VERSION_H"
    render_sbom < "$SBOM" > "$SBOM.tmp" && mv "$SBOM.tmp" "$SBOM"
    echo "version_sync: generated $VERSION_H and $SBOM for version $VSTR"
}

fail=0
do_check() {
    parse_version "$(cat "$VERSION_FILE")"
    # version.h must match byte-for-byte what VERSION generates.
    if ! render_version_h | cmp -s - "$VERSION_H"; then
        echo "version-drift: $VERSION_H does not match VERSION ($VSTR); run 'make version-sync'"; fail=1
    fi
    # SBOM keel component version + purl must match VERSION.
    if ! render_sbom < "$SBOM" | cmp -s - "$SBOM"; then
        echo "version-drift: $SBOM keel component version/purl does not match VERSION ($VSTR)"; fail=1
    fi
    # keel.pc.in must carry the @VERSION@ placeholder and NO literal version.
    if ! grep -q '^Version: @VERSION@$' "$PC_IN"; then
        echo "version-drift: $PC_IN must have 'Version: @VERSION@' (single-source, substituted at build)"; fail=1
    fi
    if grep -Eq '^Version: [0-9]' "$PC_IN"; then
        echo "version-drift: $PC_IN has a hardcoded version literal (must be @VERSION@)"; fail=1
    fi
    # No independent KL_VERSION_* literals may live in the umbrellas (they must include version.h).
    for h in include/keel/keel.h include/keel/freestanding.h; do
        if grep -Eq '#define[[:space:]]+KL_VERSION_(MAJOR|MINOR|PATCH|STRING|NUMBER)' "$h"; then
            echo "version-drift: $h defines KL_VERSION_* locally (must #include <keel/version.h>)"; fail=1
        fi
    done
    if [ "$fail" = 0 ]; then
        echo "check-version-drift: OK (VERSION=$VSTR; version.h, keel.pc.in, sbom all agree; no stray literals)"
    fi
    return "$fail"
}

# positive + negative canaries on a throwaway copy: prove parse/generate is correct for a normal AND a
# prerelease version, and that --check FAILS on an injected drift.
do_selftest() {
    st=0
    # positive: normal
    parse_version "2.9.0"
    [ "$MAJOR.$MINOR.$PATCH" = "2.9.0" ] && [ "$VNUM" = "20900" ] && [ "$ISPRE" = "0" ] && [ -z "$PRERELEASE" ] \
        || { echo "selftest: normal parse FAILED"; st=1; }
    render_version_h | grep -q '#define KL_VERSION_STRING "2.9.0"' || { echo "selftest: normal version.h FAILED"; st=1; }
    render_version_h | grep -q '#define KL_VERSION_IS_PRERELEASE 0' || { echo "selftest: normal ISPRE FAILED"; st=1; }
    # positive: prerelease
    parse_version "3.0.0-rc.1"
    [ "$MAJOR.$MINOR.$PATCH" = "3.0.0" ] && [ "$VNUM" = "30000" ] && [ "$ISPRE" = "1" ] && [ "$PRERELEASE" = "rc.1" ] \
        || { echo "selftest: prerelease parse FAILED"; st=1; }
    render_version_h | grep -q '#define KL_VERSION_STRING "3.0.0-rc.1"' || { echo "selftest: prerelease STRING FAILED"; st=1; }
    render_version_h | grep -q '#define KL_VERSION_PRERELEASE "rc.1"' || { echo "selftest: prerelease PRERELEASE FAILED"; st=1; }
    render_version_h | grep -q '#define KL_VERSION_NUMBER' || { echo "selftest: NUMBER macro FAILED"; st=1; }
    render_version_h | grep -q '#define KL_VERSION_IS_PRERELEASE 1' || { echo "selftest: prerelease ISPRE FAILED"; st=1; }
    # negative: malformed versions must be rejected
    for bad in "2.9" "x.y.z" "" "2.9.0.1"; do
        if parse_version "$bad" 2>/dev/null; then echo "selftest: malformed '$bad' wrongly accepted"; st=1; fi
    done
    # negative: an injected drift in version.h must be caught by --check
    tmpd=$(mktemp -d)
    cp "$VERSION_H" "$tmpd/orig.h"
    printf '#define KL_VERSION_STRING "9.9.9"\n' >> "$VERSION_H"
    if sh "$0" --check >/dev/null 2>&1; then echo "selftest: injected version.h drift NOT caught"; st=1; fi
    cp "$tmpd/orig.h" "$VERSION_H"; rm -rf "$tmpd"
    [ "$st" = 0 ] && echo "version_sync selftest: OK (normal + prerelease parse/generate; drift + malformed rejected)"
    return "$st"
}

case "${1:---check}" in
    --generate) do_generate ;;
    --check)    do_check ;;
    --selftest) do_selftest ;;
    *) echo "usage: $0 [--generate|--check|--selftest]" >&2; exit 2 ;;
esac
