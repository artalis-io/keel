#!/bin/sh
# tools/f2_standalone_headers.sh - F2-1b standalone installed-header compilation.
#
# Stage-installs the public headers, then proves EACH installed <keel/*.h> compiles ALONE (no other
# Keel header included first) under both C11 and C++11. A header that silently depends on another
# header's prior inclusion, or that is not self-contained, fails here. Self-canaried: a negative probe
# that references an undeclared symbol MUST fail to compile, proving the compile actually type-checks
# (not a no-op). C11 always; C++11 where a C++ compiler exists.
set -eu

ROOT=$(git rev-parse --show-toplevel 2>/dev/null || echo .)
cd "$ROOT"
CC=${CC:-cc}
CXX=${CXX:-c++}

stage=$(mktemp -d)
tmp=$(mktemp -d)
trap 'rm -rf "$stage" "$tmp"; rm -f "$ROOT/keel.pc"' EXIT INT TERM

rm -f "$ROOT/keel.pc"                        # force keel.pc regeneration for the staged prefix
make -s install PREFIX="$stage" >/dev/null 2>&1
INC="$stage/include"
[ -f "$INC/keel/keel.h" ] || { echo "standalone-headers: staged headers missing"; exit 2; }

have_cxx=0
command -v "$CXX" >/dev/null 2>&1 && have_cxx=1

bad=0
n=0
for h in "$INC"/keel/*.h; do
    rel=${h#"$INC"/}
    n=$((n + 1))
    printf '#include <%s>\nint main(void){return 0;}\n' "$rel" > "$tmp/one.c"
    cp "$tmp/one.c" "$tmp/one.cpp"
    if ! (cd "$tmp" && $CC -std=c11 -I"$INC" -fsyntax-only one.c) 2>/dev/null; then
        echo "standalone-headers: C11 FAILED for <$rel> (not self-contained)"; bad=1
    fi
    if [ "$have_cxx" = 1 ]; then
        if ! (cd "$tmp" && $CXX -std=c++11 -I"$INC" -fsyntax-only one.cpp) 2>/dev/null; then
            echo "standalone-headers: C++11 FAILED for <$rel> (not self-contained)"; bad=1
        fi
    fi
done

# Negative canary: including a real header but referencing an undeclared symbol MUST fail, proving the
# compile above is a genuine type-check (a no-op toolchain would let this pass).
printf '#include <keel/error.h>\nint main(void){ return kl_no_such_symbol_canary(); }\n' > "$tmp/neg.c"
if (cd "$tmp" && $CC -std=c11 -I"$INC" -fsyntax-only neg.c) 2>/dev/null; then
    echo "standalone-headers: NEGATIVE CANARY compiled but must fail (compile is not type-checking)"; bad=1
else
    echo "standalone-headers: negative canary correctly rejected (undeclared symbol fails to compile)"
fi

if [ "$bad" = 0 ]; then
    cxxnote="C11 only (no C++ compiler)"
    [ "$have_cxx" = 1 ] && cxxnote="C11 and C++11"
    echo "standalone-headers: OK ($n installed keel/*.h each compile alone under $cxxnote)"
fi
exit "$bad"
