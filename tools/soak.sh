#!/bin/sh
# soak.sh - run one make test target many times and tally what failed, crashed or hung.
#
# Usage:  soak.sh <rounds> <per-round-timeout-seconds> <make arguments...>
#   e.g.  soak.sh 20 900 BACKEND=iouring test-iouring
#
# Intermittent failures are what one CI run per change misses: the io_uring EINTR flake
# (ws_split_frames, 4 in 120 runs) and the server start-up stop race (one hang in many runs) were
# both found by running the same suites again and again. The tests are built by the first round and
# merely re-run after that (make sees them up to date).
#
# A round that exceeds the timeout is a HANG: its last lines are kept, because a killed test binary
# loses its buffered stdout (the test names) and only stderr (a server's "listening" lines) survives.
# Prints a per-test tally and exits non-zero if any round failed, crashed or hung.
set -u

rounds=$1; limit=$2; shift 2
out=${SOAK_DIR:-soak-out}
mkdir -p "$out"
# GNU timeout, or gtimeout (Homebrew coreutils on macOS). Without either a hang is never reported
# as one: the job's own time limit kills it instead, with no tally.
tmo=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
[ -n "$tmo" ] || echo "soak: no timeout/gtimeout: rounds run without a time limit" >&2
: > "$out/failures.txt"
bad=0

i=1
while [ "$i" -le "$rounds" ]; do
    log="$out/round-$i.log"
    if [ -n "$tmo" ]; then
        "$tmo" "$limit" make "$@" > "$log" 2>&1; rc=$?
    else
        make "$@" > "$log" 2>&1; rc=$?
    fi
    # A sanitizer report fails the round even when the binary exited 0 (a recovering UBSan build).
    if [ "$rc" -eq 0 ] && grep -qE 'runtime error:|SUMMARY: [A-Za-z]+Sanitizer' "$log"; then
        rc=99
    fi
    if [ "$rc" -eq 124 ]; then
        echo "HANG round $i" >> "$out/failures.txt"
        echo "=== round $i HANG (killed after ${limit}s); last lines:"; tail -n 15 "$log"
        bad=$((bad + 1))
    elif [ "$rc" -ne 0 ]; then
        # One line per failed test (utest prints it with the duration once, then again in a list).
        grep -E '^\[  FAILED  \] [a-z_0-9]+\.[a-z_0-9]+ \(' "$log" \
            | sed -E 's/^\[  FAILED  \] ([a-z_0-9.]+) .*/FAIL \1/' >> "$out/failures.txt"
        grep -E 'Segmentation fault|Aborted|SUMMARY: [A-Za-z]+Sanitizer|runtime error:' "$log" \
            | sed -E "s/^/CRASH round $i: /" >> "$out/failures.txt"
        echo "=== round $i failed (rc $rc)"
        bad=$((bad + 1))
    else
        echo "=== round $i ok"
        [ "$i" -gt 1 ] && rm -f "$log"   # keep the first log (the build) and every failing one
    fi
    i=$((i + 1))
done

echo
echo "soak: $bad of $rounds rounds failed for: make $*"
if [ -s "$out/failures.txt" ]; then
    echo "per test (count of rounds):"
    sort "$out/failures.txt" | uniq -c | sort -rn
fi
[ "$bad" -eq 0 ]
