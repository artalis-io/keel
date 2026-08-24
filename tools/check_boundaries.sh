#!/bin/sh
# tools/check_boundaries.sh: R4 include-classification boundary gates (frozen §6.2): G1/G2/G3.
#
# Extracts EVERY #include token (BOTH <...> and "...") and classifies its basename/path against
# inventories DERIVED FROM THE TREE (not fixed regex lists). This closes two bypasses the earlier
# fixed-list gates had:
#   * an include-search-path escape: `#include <http_internal.h>` / `#include "x_internal.h"` (bare,
#     resolved via -Isrc) was invisible to a quoted-path-shaped scan; now every token is classified.
#   * a deny-list blind spot: a NEWLY named integration/protocol header absent from a fixed regex was
#     silently allowed; the inventories are now derived from the actual tree, so new names are covered.
#
# Default-deny. Positive+negative self-canary per mode. file:line diagnostics. Binary files skipped.
# Usage: check_boundaries.sh G1|G2|G3
set -eu

mode="${1:?usage: check_boundaries.sh G1|G2|G3}"

# Frozen §6.3 integration->substrate seam allowlist (EXACT basenames; longest-first where a prefix
# collides). The ONLY private core headers an integration may include. A new seam is added here by
# exact name, with a rationale; never a wildcard.
seam_allow_alt='socket|platform|completion_io|completion|event_caps|event_builtin|sockaddr_native|sockcompat|watcher_internal|resolve_sync|datagram_life|datagram_open|udp_cmsg_win|udp_cmsg'

# Build a '|'-joined, dot-escaped alternation of the BASENAMES of the paths read on stdin.
basenames_re() { sed 's#.*/##' | sort -u | sed 's/\./\\./g' | paste -sd'|' - ; }

# Emit "file:line:target" for every #include (both <...> and "...") in the given files. -I skips
# binaries; the sed strips the `#include <`/`"` prefix and the closing `>`/`"`, leaving the raw target.
includes() {
  [ "$#" -eq 0 ] && return 0
  grep -HnoIE '#[[:space:]]*include[[:space:]]*[<"][^>"]*[>"]' "$@" 2>/dev/null \
    | sed -E 's/#[[:space:]]*include[[:space:]]*[<"]//; s/[>"]$//'
}

fail=0

case "$mode" in
  G1)
    # Substrate = bare src/*.{c,h} (src/ minus src/protocols/**). Forbid any protocol header: a
    # src/protocols/** path OR a protocol-private-header basename (derived from src/protocols/*/*.h).
    proto_re=$(git ls-files 'src/protocols/*/*.h' | basenames_re); [ -z "$proto_re" ] && proto_re='__NEVER__'
    printf 'f:1:http_internal.h\n' | grep -qE "[:/]($proto_re)\$" || { echo "check-substrate-purity: SELF-TEST FAILED; protocol basename not detected"; exit 1; }
    printf 'f:1:completion.h\n'    | grep -qE "[:/]($proto_re)\$" && { echo "check-substrate-purity: SELF-TEST FAILED; substrate completion.h flagged as protocol"; exit 1; }
    hits=$(includes src/*.c src/*.h | grep -E "(protocols/|[:/]($proto_re)\$)" || true)
    if [ -n "$hits" ]; then echo "$hits"; echo "check-substrate-purity: FAILED (G1); a substrate TU includes a protocol header (src/protocols/** path or protocol-private basename)"; fail=1; fi
    [ $fail -eq 0 ] && echo "check-substrate-purity: OK (G1; no src/*.{c,h} include, <> or \"\", resolves to a protocol header)"
    ;;
  G2)
    # Protocols = src/protocols/*/*.{c,h}. Forbid any integration-owned ADAPTER header (derived from
    # integrations/**/*.h). Exclude mbedtls_shim/; those are system-header SHADOWS (stdio.h/time.h/…),
    # not adapter headers, so a protocol's <stdio.h> must not be misclassified.
    integ_re=$(git ls-files 'integrations/*' | grep -E '\.h$' | grep -vE '/mbedtls_shim/' | basenames_re); [ -z "$integ_re" ] && integ_re='__NEVER__'
    printf 'f:1:keel_tls_mbedtls.h\n' | grep -qE "[:/]($integ_re)\$" || { echo "check-protocol-no-integration: SELF-TEST FAILED; integration adapter header not detected"; exit 1; }
    printf 'f:1:http_internal.h\n'    | grep -qE "[:/]($integ_re)\$" && { echo "check-protocol-no-integration: SELF-TEST FAILED; a protocol header misclassified as integration"; exit 1; }
    set -- $(git ls-files 'src/protocols/*/*.c' 'src/protocols/*/*.h')
    hits=$(includes "$@" | grep -E "[:/]($integ_re)\$" || true)
    if [ -n "$hits" ]; then echo "$hits"; echo "check-protocol-no-integration: FAILED (G2); a protocol TU includes an integration adapter header"; fail=1; fi
    [ $fail -eq 0 ] && echo "check-protocol-no-integration: OK (G2; no src/protocols include, <> or \"\", resolves to an integration adapter header)"
    ;;
  G3)
    # Integrations. Allowed core includes = ONLY the frozen §6.3 seam allowlist. Any OTHER private
    # core header (basename derived from src/*.h + src/protocols/*/*.h) is a violation, as is any
    # src/protocols/** path. Public <keel/*.h>, integration-local, and system headers are unaffected
    # (their basenames are not in the core inventory).
    core_re=$(git ls-files 'src/*.h' 'src/protocols/*/*.h' | basenames_re); [ -z "$core_re" ] && core_re='__NEVER__'
    # self-test: an allowlisted seam is NOT a violation; a real non-allowlisted core header IS
    # (the negative sample is derived from the tree, so it stays valid across header renames).
    sample=$(git ls-files 'src/*.h' 'src/protocols/*/*.h' | sed 's#.*/##' | grep -vE "^($seam_allow_alt)\.h\$" | head -1)
    printf 'f:1:../../src/socket.h\n'         | grep -E "[:/]($core_re)\$" | grep -vE "[:/]($seam_allow_alt)\.h\$" | grep -q . && { echo "check-integration-seam: SELF-TEST FAILED; allowlisted socket.h flagged"; exit 1; }
    printf 'f:1:../../src/%s\n' "$sample"     | grep -E "[:/]($core_re)\$" | grep -vE "[:/]($seam_allow_alt)\.h\$" | grep -q . || { echo "check-integration-seam: SELF-TEST FAILED; a non-allowlisted core header ($sample) slipped past"; exit 1; }
    set -- $(git ls-files 'integrations/*' | grep -E '\.(c|h)$')
    incs=$(includes "$@")
    prot=$(printf '%s\n' "$incs" | grep -E 'protocols/' || true)
    if [ -n "$prot" ]; then echo "$prot"; echo "check-integration-seam: FAILED (G3); an integration includes a src/protocols/** protocol header"; fail=1; fi
    badseam=$(printf '%s\n' "$incs" | grep -E "[:/]($core_re)\$" | grep -vE "[:/]($seam_allow_alt)\.h\$" || true)
    if [ -n "$badseam" ]; then echo "$badseam"; echo "check-integration-seam: FAILED (G3); an integration includes a private core header outside the frozen §6.3 seam allowlist"; fail=1; fi
    [ $fail -eq 0 ] && echo "check-integration-seam: OK (G3; every integration core include, <> or \"\", is a frozen §6.3 seam header; no protocol header)"
    ;;
  *)
    echo "check_boundaries.sh: unknown mode '$mode' (want G1|G2|G3)"; exit 2 ;;
esac

exit $fail
