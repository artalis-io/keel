#!/bin/sh
# tools/f2_cxx_link_test.sh - F2-A staged C++ compile-and-link coverage against the C-built archive.
#
# Proves that Keel's public headers, carrying consistent extern "C" guards, can be included AND linked
# from C++ against the C-compiled libkeel.a. The contract is C linkage for C++ consumers, not a C++
# wrapper API.
#
# Steps:
#   1. Build the C archive (libkeel.a).
#   2. Stage-install into a temp prefix (keel.pc regenerated for that prefix; see the F2 staleness
#      finding) and derive flags via pkg-config only (no repository-private include path).
#   3. POSITIVE: compile+link a two-TU C++ consumer (representative substrate, HTTP, datagram, DNS,
#      and freestanding-umbrella calls) from OUTSIDE the repo, then run it. Must succeed and exit 0.
#   4. NEGATIVE canary: compile+link a C++ TU that hand-declares a Keel function WITHOUT extern "C".
#      Its mangled reference cannot resolve the archive's C symbol, so the link MUST FAIL.
#
# Exit 0 iff the positive consumer links+runs AND the negative canary fails to link.
set -eu

ROOT=$(git rev-parse --show-toplevel 2>/dev/null || echo .)
cd "$ROOT"
SRC="$ROOT/tools/f2_cxx_link"
CXX=${CXX:-c++}
CXXSTD=${CXXSTD:-c++11}

command -v pkg-config >/dev/null 2>&1 || { echo "cxx-link: pkg-config not found"; exit 2; }

echo "cxx-link: building libkeel.a"
make -s libkeel.a >/dev/null 2>&1 || make -s >/dev/null 2>&1

stage=$(mktemp -d)
build=$(mktemp -d)
trap 'rm -rf "$stage" "$build"; rm -f "$ROOT/keel.pc"' EXIT INT TERM

echo "cxx-link: staging install into $stage"
rm -f "$ROOT/keel.pc"                     # force keel.pc regeneration for the staged prefix
make -s install PREFIX="$stage" >/dev/null 2>&1

PKG_CONFIG_PATH="$stage/lib/pkgconfig"; export PKG_CONFIG_PATH
pkg-config --exists keel || { echo "cxx-link: staged keel.pc not found"; exit 1; }
CFLAGS=$(pkg-config --cflags keel)
LIBS=$(pkg-config --libs --static keel)

# Guard against a repository-private include leak: the staged cflags must not reference the source tree.
case "$CFLAGS" in *"$ROOT/include"*|*" -Iinclude"*) echo "cxx-link: staged cflags leak the source include path"; exit 1;; esac

bad=0

# POSITIVE: two-TU consumer, built from an out-of-repo directory, staged flags only.
echo "cxx-link: POSITIVE compile+link+run"
# shellcheck disable=SC2086
if (cd "$build" && $CXX -std=$CXXSTD $CFLAGS "$SRC/positive.cpp" "$SRC/freestanding.cpp" $LIBS -o pos) 2>"$build/pos.err"; then
    if "$build/pos"; then echo "  positive: linked and ran"; else echo "  positive: FAILED at runtime"; bad=1; fi
else
    echo "  positive: FAILED to compile/link"; sed -n '1,8p' "$build/pos.err"; bad=1
fi

# POSITIVE (C): a plain C consumer that exercises the kl_event_dispatch inline (which references the
# internal keel__event_dispatch_watcher link-support symbol), linked against the C archive and run.
echo "cxx-link: POSITIVE C consumer compile+link+run"
CC=${CC:-cc}
# shellcheck disable=SC2086
if (cd "$build" && $CC -std=c11 $CFLAGS "$SRC/dispatch_c.c" $LIBS -o dispatch_c) 2>"$build/c.err"; then
    if "$build/dispatch_c" >/dev/null; then echo "  C consumer: linked and ran"; else echo "  C consumer: FAILED at runtime"; bad=1; fi
else
    echo "  C consumer: FAILED to compile/link"; sed -n '1,8p' "$build/c.err"; bad=1
fi

# NEGATIVE canary: unguarded C++ declaration must NOT link against the C archive.
echo "cxx-link: NEGATIVE canary (must fail to link)"
# shellcheck disable=SC2086
if (cd "$build" && $CXX -std=$CXXSTD $CFLAGS "$SRC/negative.cpp" $LIBS -o neg) 2>"$build/neg.err"; then
    echo "  negative: LINKED but should have failed (extern \"C\" guard is not doing its job)"; bad=1
else
    echo "  negative: correctly failed to link (unguarded C++ decl cannot resolve the C symbol)"
fi

[ "$bad" = 0 ] && echo "cxx-link: OK (C++ links against the C archive with guards; unguarded linkage fails)"
exit "$bad"
