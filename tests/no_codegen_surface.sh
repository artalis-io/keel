#!/bin/sh
# no_codegen_surface.sh — W^X regression guard
#
# Keel's W^X invariant (SECURITY.md "Architectural Guarantees"): the
# library has no path that creates executable memory or loads native
# code at runtime. This script fails the build if any future commit
# introduces a syscall or API that would violate the invariant.
#
# Run manually:   sh tests/no_codegen_surface.sh
# Run from CI:    make wx-guard  (or invoke the script directly)
#
# Exit 0 = clean. Exit 1 = at least one forbidden symbol was found
# under src/ or parsers/.
#
# SPDX-License-Identifier: MIT

set -e

# Resolve repo root from this script's location so the test works
# regardless of cwd (CI invokes it as `sh tests/no_codegen_surface.sh`
# from the repo root; developers might run it from anywhere).
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

# Symbols whose presence in Keel sources would weaken W^X.
#
# We grep for whole-word identifiers; comments mentioning these symbols
# are fine (and useful), so the patterns require a non-identifier char
# on at least one side. POSIX BRE doesn't have \b — we use POSIX ERE
# via `grep -E` with explicit boundary characters.
#
# The first three are runtime executable-memory primitives. The next
# three are dynamic-native-load primitives. The last two are
# subprocess invocation, which Hull's pledge promise set forbids and
# Keel must therefore not introduce either.
PATTERNS='(^|[^A-Za-z0-9_])(mmap|mprotect)[[:space:]]*\([^)]*PROT_EXEC|MAP_JIT|memfd_create|pthread_jit_write_protect|dlopen|dlsym|(^|[^A-Za-z0-9_])(popen|system|execve|execvp|execl|execle|execlp|execvpe)[[:space:]]*\('

# Files to scan. We deliberately exclude vendor/ — llhttp et al. are
# third-party and tracked separately in SBOM. Tests, examples, and
# fuzz harnesses are also excluded; they may legitimately exercise
# these symbols (e.g. an example might fork a helper process). The
# invariant is about the library, not its test surface.
SCAN_PATHS="$ROOT/src $ROOT/parsers"

# Quietly verify the paths exist (defensive — the script's only job
# is to fail on regression, so missing dirs should fail too).
for p in $SCAN_PATHS; do
    if [ ! -d "$p" ]; then
        echo "no_codegen_surface: scan path missing: $p" >&2
        exit 1
    fi
done

# Use `find ... -print0 | xargs -0 grep` to handle exotic file names
# robustly. The `|| true` on grep keeps `set -e` from killing us when
# grep finds nothing (exit 1 is its "no match" return).
HITS=$(find $SCAN_PATHS -type f \( -name '*.c' -o -name '*.h' \) -print0 \
       | xargs -0 grep -EHn "$PATTERNS" 2>/dev/null || true)

if [ -n "$HITS" ]; then
    echo "no_codegen_surface: FAIL — forbidden W^X surface found:"
    echo "$HITS"
    echo ""
    echo "These symbols would create executable memory, load native"
    echo "code at runtime, or invoke subprocesses. Keel's W^X policy"
    echo "(SECURITY.md \"Architectural Guarantees\") forbids them in"
    echo "library sources. If this is intentional, update SECURITY.md"
    echo "before merging."
    exit 1
fi

echo "no_codegen_surface: PASS — no forbidden W^X surface in src/ or parsers/"
exit 0
