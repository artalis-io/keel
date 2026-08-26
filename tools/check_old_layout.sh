#!/bin/sh
# tools/check_old_layout.sh - S4 check-old-layout: the single authoritative resurrection gate.
#
# Rejects re-introduction of every deleted layout the restructure removed. Default-deny, self-
# canaried per case, file:line diagnostics, binary-skip, BSD+GNU-portable. Structural checks run
# over `git ls-files` (tracked paths); reference checks run over a curated code + living-docs scan
# set - design/freeze/roadmap/audit docs and this gate's own files are excluded, because they
# legitimately name old paths (same self-reference handling as the other stale-name gates).
#
# Protocol test placement inside the tests/ tree is owned by the companion gate check-test-layout
# (tests/test-layout.manifest, exact default-deny for both root and nested tests); this gate does
# not reimplement it. Integration test placement is enforced by case 5's ownership manifest below.
set -eu

# Curated reference scan set (inherited from the retired check-no-interim-paths). Historical docs
# and the gate's own sources are outside it.
SCAN='src include tests examples bench fuzz integrations README.md CLAUDE.md AGENTS.md CONTRIBUTING.md docs/architecture/overview.md docs/architecture/invariants.md docs/operations/capability_matrix.md'

fail=0

# ── Case 1 - top-level protocols/ (structural + reference) ─────────────────────
if git ls-files 'protocols/*' | grep -q .; then
  git ls-files 'protocols/*' | sed 's/^/  tracked: /'
  echo "check-old-layout: FAILED (case 1) - a tracked file lives under a resurrected top-level protocols/ (final home is src/protocols/)"; fail=1
fi
INTERIM_RE='([A-Za-z0-9_./]*)protocols/(http2|http|websocket|dns|proxy_protocol)/'
printf 'f:1:protocols/http/x\n'     | grep -oE "$INTERIM_RE" | grep -vE '(src|tests)/protocols/' | grep -q . || { echo "check-old-layout: SELF-TEST FAILED (case 1 - bare interim path not caught)"; exit 1; }
printf 'f:1:src/protocols/http/x\n' | grep -oE "$INTERIM_RE" | grep -vE '(src|tests)/protocols/' | grep -q . && { echo "check-old-layout: SELF-TEST FAILED (case 1 - final src/protocols/ path flagged)"; exit 1; }
interim=$(grep -rInoE "$INTERIM_RE" $SCAN 2>/dev/null | grep -vE '(src|tests)/protocols/[A-Za-z0-9_]+/$' || true)
if [ -n "$interim" ]; then echo "$interim"; echo "check-old-layout: FAILED (case 1) - a reference to the interim top-level protocols/ layout (final layout is src/protocols/)"; fail=1; fi

# ── Case 2 - old flat integration homes (structural allowlist + reference) ─────
# Structural: every direct child of integrations/ must be an allowlisted role dir or umbrella file.
# A new architectural role is a deliberate, reviewed edit to this allowlist with a rationale.
ROLE_ALLOW_RE='^(codec|http2|platform|tls|Makefile|README\.md|non-test-files\.manifest)$'
printf 'lwip\n' | grep -vE "$ROLE_ALLOW_RE" | grep -q . || { echo "check-old-layout: SELF-TEST FAILED (case 2 - a non-role child 'lwip' is not flagged by the allowlist)"; exit 1; }
printf 'tls\n'  | grep -vE "$ROLE_ALLOW_RE" | grep -q . && { echo "check-old-layout: SELF-TEST FAILED (case 2 - the allowed role 'tls' is flagged)"; exit 1; }
children=$(git ls-files 'integrations/*' | sed -E 's#^integrations/##; s#/.*##' | sort -u)
badchild=$(printf '%s\n' "$children" | grep -vE "$ROLE_ALLOW_RE" || true)
if [ -n "$badchild" ]; then printf '  integrations/%s\n' $badchild; echo "check-old-layout: FAILED (case 2) - a non-role entry directly under integrations/ (allowed roles: codec http2 platform tls; a new role needs a reviewed gate-allowlist change)"; fail=1; fi
OLD_RE='integrations/(lwip|uefi|mbedtls|openssl|boringssl|libressl|nghttp2|miniz)/'
printf 'x integrations/lwip/y\n'          | grep -qE "$OLD_RE" || { echo "check-old-layout: SELF-TEST FAILED (case 2 - old flat backend path not caught)"; exit 1; }
printf 'x integrations/platform/lwip/y\n' | grep -qE "$OLD_RE" && { echo "check-old-layout: SELF-TEST FAILED (case 2 - a role-nested backend path flagged)"; exit 1; }
oldhome=$(grep -rInIE "$OLD_RE" $SCAN 2>/dev/null || true)
if [ -n "$oldhome" ]; then echo "$oldhome"; echo "check-old-layout: FAILED (case 2) - a reference to an old flat integration home (final layout is integrations/<role>/<backend>/)"; fail=1; fi

# ── Case 3 - spikes/ (structural + reference) ──────────────────────────────────
if git ls-files 'spikes/*' | grep -q .; then git ls-files 'spikes/*' | sed 's/^/  tracked: /'; echo "check-old-layout: FAILED (case 3) - a tracked file lives under a resurrected spikes/"; fail=1; fi
printf 'x spikes/uefi/y\n' | grep -qE '\bspikes/' || { echo "check-old-layout: SELF-TEST FAILED (case 3 - spikes/ path not caught)"; exit 1; }
printf 'x src/foo/y\n'     | grep -qE '\bspikes/' && { echo "check-old-layout: SELF-TEST FAILED (case 3 - a non-spikes path flagged)"; exit 1; }
spikes=$(grep -rInIE '\bspikes/' $SCAN 2>/dev/null || true)
if [ -n "$spikes" ]; then echo "$spikes"; echo "check-old-layout: FAILED (case 3) - a reference to the removed spikes/ tree"; fail=1; fi

# ── Case 4 - deleted directory paths: parsers/ (structural + reference) ────────
if git ls-files 'parsers/*' | grep -q .; then git ls-files 'parsers/*' | sed 's/^/  tracked: /'; echo "check-old-layout: FAILED (case 4) - a tracked file lives under a resurrected parsers/ (now src/protocols/http/)"; fail=1; fi
printf 'x parsers/http1_x.c\n'                    | grep -qE '\bparsers/http1' || { echo "check-old-layout: SELF-TEST FAILED (case 4 - parsers/http1 path not caught)"; exit 1; }
printf 'x src/protocols/http/http1_parser.c\n'    | grep -qE '\bparsers/http1' && { echo "check-old-layout: SELF-TEST FAILED (case 4 - the retained src/protocols/http parser path flagged)"; exit 1; }
parsers=$(grep -rInIE '\bparsers/http1' $SCAN 2>/dev/null || true)
if [ -n "$parsers" ]; then echo "$parsers"; echo "check-old-layout: FAILED (case 4) - a reference to the deleted parsers/http1 path (now src/protocols/http/)"; fail=1; fi

# ── Case 5 - integration tests outside a backend tests/: exact ownership manifest ──
# Test content is EXACTLY integrations/<role>/<backend>/tests/... ; every other file under
# integrations/ (including a tests/ at the wrong depth - role-level, or nested below the backend)
# is a non-test file that must be manifest-declared.
MANIFEST=integrations/non-test-files.manifest
TESTS_RE='^integrations/(codec|http2|platform|tls)/[^/]+/tests/'
# partition canaries (two-direction): a backend-root file is NON-test (not exempt); a correctly
# placed backend test IS exempt; a tests/ at the wrong depth is NOT exempt.
printf 'integrations/tls/mbedtls/probe.c\n'             | grep -qE "$TESTS_RE" && { echo "check-old-layout: SELF-TEST FAILED (case 5 - a backend-root file was exempted as test content)"; exit 1; }
printf 'integrations/tls/mbedtls/tests/probe.c\n'       | grep -qE "$TESTS_RE" || { echo "check-old-layout: SELF-TEST FAILED (case 5 - a correctly-placed backend test was not exempted)"; exit 1; }
printf 'integrations/tls/tests/probe.c\n'               | grep -qE "$TESTS_RE" && { echo "check-old-layout: SELF-TEST FAILED (case 5 - a role-level tests/ was wrongly exempted)"; exit 1; }
printf 'integrations/tls/mbedtls/extra/tests/probe.c\n' | grep -qE "$TESTS_RE" && { echo "check-old-layout: SELF-TEST FAILED (case 5 - a tests/ nested below the backend was wrongly exempted)"; exit 1; }
git ls-files 'integrations/*' | grep -vE "$TESTS_RE" | LC_ALL=C sort > /tmp/keel_ol_nontest.$$
grep -vE '^[[:space:]]*(#|$)' "$MANIFEST" | LC_ALL=C sort > /tmp/keel_ol_manifest.$$
unclassified=$(grep -vxFf /tmp/keel_ol_manifest.$$ /tmp/keel_ol_nontest.$$ || true)
if [ -n "$unclassified" ]; then echo "$unclassified"; echo "check-old-layout: FAILED (case 5) - a backend-root integration file is not declared in $MANIFEST (declare it there, or move it under the backend's tests/)"; fail=1; fi
stale=$(grep -vxFf /tmp/keel_ol_nontest.$$ /tmp/keel_ol_manifest.$$ || true)
if [ -n "$stale" ]; then echo "$stale"; echo "check-old-layout: FAILED (case 5) - $MANIFEST lists a file that is no longer a tracked backend-root file"; fail=1; fi
rm -f /tmp/keel_ol_nontest.$$ /tmp/keel_ol_manifest.$$

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "check-old-layout: OK - no resurrected protocols/ | integrations flat-home | spikes/ | parsers/ layout; every integration non-test file is manifest-declared. (Protocol test placement in tests/ is owned by check-test-layout.)"
