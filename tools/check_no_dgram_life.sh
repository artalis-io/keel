#!/bin/sh
# tools/check_no_dgram_life.sh - stale-name gate for the completion-lifetime token.
#
# The token that pins completion-op storage past its owner (src/completion_life.{h,c}, KlCompLife,
# kl_comp_life_*) was first written for datagrams and carried a datagram name. Windows Named Pipe
# streams became its second owner, which made that name false, so it was renamed. This gate keeps the
# old names from coming back in code or living docs, where they would again suggest the token is
# datagram-specific.
#
# Scan set: every tracked file except docs/archive/ (historical records keep the names they were
# written with), vendor/, and this script (which must spell the patterns). Default-deny: a new file
# is covered automatically. Self-canaried.
# Usage: tools/check_no_dgram_life.sh [--selftest]
set -eu

OLD_RE='\bKlDgram''Life\b|\bKlDgram''DispatchFn\b|\bkl_dgram''_life_[a-z_]*|datagram''_life'

selftest() {
    fail=0
    hit()  { printf '%s\n' "$1" | grep -qE "$OLD_RE" || { echo "check-no-dgram-life: SELF-TEST FAILED: missed '$1'"; fail=1; }; }
    miss() { printf '%s\n' "$1" | grep -qE "$OLD_RE" && { echo "check-no-dgram-life: SELF-TEST FAILED: false hit on '$1'"; fail=1; } || true; }
    hit  "KlDgram""Life *l = x;"
    hit  "typedef void (*KlDgram""DispatchFn)(void);"
    hit  "kl_dgram""_life_release(l);"
    hit  '#include "datagram''_life.h"'
    hit  "tests/test_datagram""_life.c"
    hit  "IOURING_TEST_SUITES = datagram""_life datagram_live"
    miss "KlCompLife *l = kl_comp_life_create(a, t, f, c, d);"
    miss "datagram_live datagram_open KlDgramCore kl_dgram_core_init KlDgramOpKind"
    [ $fail -eq 0 ] || exit 1
}

selftest
[ "${1:-}" = "--selftest" ] && { echo "check-no-dgram-life: selftest OK"; exit 0; }

hits=$(git ls-files -z | tr '\0' '\n' \
    | grep -vE '^(docs/archive/|vendor/)' | grep -vxF 'tools/check_no_dgram_life.sh' \
    | while IFS= read -r f; do
          [ -f "$f" ] || continue
          grep -InE "$OLD_RE" "$f" 2>/dev/null | sed "s|^|$f:|" || true
      done)
if [ -n "$hits" ]; then
    printf '%s\n' "$hits"
    echo "check-no-dgram-life: FAILED, a datagram-era name for the completion-lifetime token reappeared; use KlCompLife / kl_comp_life_* / completion_life.{h,c}"
    exit 1
fi
echo "check-no-dgram-life: OK (no datagram-era token name in code or living docs; docs/archive excluded as history; self-canary green)"
