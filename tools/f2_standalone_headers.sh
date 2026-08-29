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

# Strict flags so an accepted compiler extension cannot hide a portability failure: -pedantic-errors
# turns every extension use into a hard error (this is how a C-only construct like _Atomic in a header
# is caught under a C++ compiler that would otherwise accept it as an extension).
STRICT="-Wall -Wextra -Werror -pedantic-errors"

bad=0
n=0
for h in "$INC"/keel/*.h; do
    rel=${h#"$INC"/}
    n=$((n + 1))
    printf '#include <%s>\nint main(void){return 0;}\n' "$rel" > "$tmp/one.c"
    cp "$tmp/one.c" "$tmp/one.cpp"
    if ! (cd "$tmp" && $CC -std=c11 $STRICT -I"$INC" -fsyntax-only one.c) 2>/dev/null; then
        echo "standalone-headers: C11 FAILED for <$rel> (not self-contained)"; bad=1
    fi
    if [ "$have_cxx" = 1 ]; then
        if ! (cd "$tmp" && $CXX -std=c++11 $STRICT -I"$INC" -fsyntax-only one.cpp) 2>/dev/null; then
            echo "standalone-headers: C++11 FAILED for <$rel> (not self-contained)"; bad=1
        fi
    fi
done

# Negative canary: declare an object of an UNDECLARED TYPE. That is a hard error in BOTH C and C++
# (unlike calling an undeclared function, which some C compilers accept with only a warning), so the
# canary reliably proves the compiles above are genuine type-checks. Checked under the C compiler and,
# where present, the C++ compiler.
printf '#include <keel/error.h>\nkl_no_such_type_canary_t kl_canary_obj;\nint main(void){return 0;}\n' > "$tmp/neg.c"
cp "$tmp/neg.c" "$tmp/neg.cpp"
if (cd "$tmp" && $CC -std=c11 $STRICT -I"$INC" -fsyntax-only neg.c) 2>/dev/null; then
    echo "standalone-headers: NEGATIVE CANARY (C) compiled but must fail (compile is not type-checking)"; bad=1
else
    echo "standalone-headers: negative canary correctly rejected by C (undeclared type)"
fi
if [ "$have_cxx" = 1 ]; then
    if (cd "$tmp" && $CXX -std=c++11 $STRICT -I"$INC" -fsyntax-only neg.cpp) 2>/dev/null; then
        echo "standalone-headers: NEGATIVE CANARY (C++) compiled but must fail (compile is not type-checking)"; bad=1
    else
        echo "standalone-headers: negative canary correctly rejected by C++ (undeclared type)"
    fi
fi

if [ "$bad" = 0 ]; then
    cxxnote="C11 only (no C++ compiler)"
    [ "$have_cxx" = 1 ] && cxxnote="C11 and C++11"
    echo "standalone-headers: OK ($n installed keel/*.h each compile alone under $cxxnote)"
fi
exit "$bad"
