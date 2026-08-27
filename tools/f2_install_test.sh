#!/bin/sh
# tools/f2_install_test.sh - F2-2 install/uninstall hardening check.
#
# Verifies, against a throwaway DESTDIR, that:
#   1. install stages exactly the reviewed public headers (docs/f2/public_headers.txt), plus the
#      library and keel.pc, under $(DESTDIR)$(PREFIX) (both semantics preserved);
#   2. keel.pc carries the requested PREFIX and regenerates when PREFIX changes (config change);
#   3. uninstall removes ONLY Keel-owned files, preserving unrelated files and a shared directory that
#      still holds them, but removing a directory Keel created once it is empty.
set -eu

ROOT=$(git rev-parse --show-toplevel 2>/dev/null || echo .)
cd "$ROOT"
MAN=docs/f2/public_headers.txt

stage=$(mktemp -d)
trap 'rm -rf "$stage"; rm -f "$ROOT/keel.pc"' EXIT INT TERM
PFX=/usr/local
D="$stage/root"

fail=0
say() { echo "install-test: $1"; }
bad() { echo "install-test: FAIL - $1"; fail=1; }

# 1. install ---------------------------------------------------------------------------------------
rm -f "$ROOT/keel.pc"
make -s install DESTDIR="$D" PREFIX="$PFX" >/dev/null 2>&1

want=$(grep -vE '^[[:space:]]*#|^[[:space:]]*$' "$MAN" | sort)
got=$(ls "$D$PFX/include/keel" 2>/dev/null | sort || true)
[ "$want" = "$got" ] || bad "installed headers differ from the manifest (default-allow leak or missing header)"
[ -f "$D$PFX/lib/libkeel.a" ] || bad "libkeel.a not installed"
[ -f "$D$PFX/lib/pkgconfig/keel.pc" ] || bad "keel.pc not installed"
grep -q "^prefix=$PFX\$" "$D$PFX/lib/pkgconfig/keel.pc" || bad "keel.pc prefix does not match PREFIX"
[ "$fail" = 0 ] && say "install stages exactly the $(printf '%s\n' "$want" | wc -l | tr -d ' ') manifested headers + lib + keel.pc"

# 2. keel.pc regeneration on config change --------------------------------------------------------
rm -f "$ROOT/keel.pc"
make -s keel.pc PREFIX=/aaa >/dev/null 2>&1; a=$(grep '^prefix=' "$ROOT/keel.pc")
make -s keel.pc PREFIX=/bbb >/dev/null 2>&1; b=$(grep '^prefix=' "$ROOT/keel.pc")
[ "$a" = "prefix=/aaa" ] && [ "$b" = "prefix=/bbb" ] || bad "keel.pc did not regenerate on a PREFIX change"
[ "$fail" = 0 ] && say "keel.pc regenerates when PREFIX changes"

# 3. uninstall preserves unrelated files ----------------------------------------------------------
touch "$D$PFX/include/keel/USER_OWNED.h" "$D$PFX/lib/pkgconfig/other.pc"
make -s uninstall DESTDIR="$D" PREFIX="$PFX" >/dev/null 2>&1
[ -f "$D$PFX/include/keel/USER_OWNED.h" ] || bad "uninstall deleted an unrelated header"
[ -d "$D$PFX/include/keel" ] || bad "uninstall removed a directory that still held an unrelated file"
[ -f "$D$PFX/lib/pkgconfig/other.pc" ] || bad "uninstall deleted an unrelated .pc"
[ -f "$D$PFX/lib/pkgconfig/keel.pc" ] && bad "uninstall left keel.pc behind"
[ -f "$D$PFX/lib/libkeel.a" ] && bad "uninstall left libkeel.a behind"
rem=$(ls "$D$PFX/include/keel" 2>/dev/null | grep -vx 'USER_OWNED.h' || true)
[ -z "$rem" ] || bad "uninstall left Keel headers behind: $rem"
[ "$fail" = 0 ] && say "uninstall removes only Keel-owned files; unrelated files and their directory survive"

# 4. clean uninstall removes an emptied directory -------------------------------------------------
rm -f "$D$PFX/include/keel/USER_OWNED.h"
make -s install DESTDIR="$D" PREFIX="$PFX" >/dev/null 2>&1
make -s uninstall DESTDIR="$D" PREFIX="$PFX" >/dev/null 2>&1
[ -d "$D$PFX/include/keel" ] && bad "uninstall did not remove the emptied keel include directory"
[ "$fail" = 0 ] && say "uninstall removes the include/keel directory once it is empty"

[ "$fail" = 0 ] && echo "install-test: OK (DESTDIR staging, manifest-only install, keel.pc regen, safe uninstall)"
exit "$fail"
