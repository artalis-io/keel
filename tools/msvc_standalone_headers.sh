#!/bin/sh
# msvc_standalone_headers.sh - every installed public header compiles ALONE under MSVC cl.
#
# The existing standalone-header gate (tools/f2_standalone_headers.sh) is GNU-only: it spells
# -fsyntax-only, -std=c11 and a C++ pass that would need /TP and different flags. Rather than rewrite a
# working gate to be bilingual, this is the MSVC-side equivalent of the same property, for the
# toolchain that now has to satisfy it.
#
# What it proves, per header: the header is self-sufficient (nothing else included first), and its
# include guard is idempotent (included twice in one TU). That is the contract an embedder relies on
# and the thing MSVC is most likely to break, because it ships a different system-header set.
#
# A NEGATIVE CANARY runs last: a TU that must fail to compile. Without it a gate that silently stopped
# finding headers, or lost its compiler, would report success for doing nothing.
#
# Usage: source scripts/msvc-env.sh, then: sh tools/msvc_standalone_headers.sh
set -e

INC=include
CL=${CL_EXE:-cl}
WORK=build/msvc-headers
# Relative paths only: with MSYS2_ARG_CONV_EXCL set (as scripts/msvc-env.sh sets it) a /tmp path would
# reach cl as the literal C:	mp, which does not exist.
rm -rf "$WORK"; mkdir -p "$WORK"

if ! "$CL" /nologo 2>/dev/null >/dev/null; then
    if ! command -v "$CL" >/dev/null 2>&1; then
        echo "msvc-standalone-headers: $CL not found; source scripts/msvc-env.sh first" >&2
        exit 1
    fi
fi

FLAGS="/nologo /c /std:c11 /experimental:c11atomics /W3 /WX /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_WARNINGS"

n=0
failed=0
for h in "$INC"/keel/*.h; do
    base=$(basename "$h")
    tu="$WORK/h_${base%.h}.c"
    # Twice, to catch a missing or mismatched include guard.
    {
        echo "#include <keel/$base>"
        echo "#include <keel/$base>"
        echo "int main(void) { return 0; }"
    } > "$tu"
    if "$CL" $FLAGS -I"$INC" "/Fo$WORK/" "$tu" >"$WORK/out.txt" 2>&1; then
        n=$((n + 1))
    else
        echo "msvc-standalone-headers: keel/$base does NOT compile alone under MSVC:"
        grep -E "error|warning" "$WORK/out.txt" | head -5
        failed=1
    fi
done

if [ "$failed" -ne 0 ]; then
    echo "msvc-standalone-headers: FAILED"
    exit 1
fi

# Negative canary: a deliberately broken TU. If this compiles, the gate is not compiling anything.
cat > "$WORK/canary.c" <<'CANARY'
#include <keel/keel.h>
int main(void) { this_identifier_does_not_exist(); return 0; }
CANARY
if "$CL" $FLAGS -I"$INC" "/Fo$WORK/" "$WORK/canary.c" >"$WORK/canary.txt" 2>&1; then
    echo "msvc-standalone-headers: SELFTEST FAILED - the canary compiled, so the gate proves nothing"
    exit 1
fi

echo "msvc-standalone-headers: OK ($n public headers compile alone and twice under MSVC; canary rejected)"
