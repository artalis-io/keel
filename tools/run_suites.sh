#!/bin/sh
# run_suites.sh - run test-suite binaries, isolating the ones that need a fresh process per test.
#
# Usage:  run_suites.sh <label> <isolated-suite-names> <binary> [<binary> ...]
#
# WHY ISOLATION EXISTS, because a one-process-per-test runner looks like pointless overhead until you
# know what it buys (#307).
#
# tests/protocols/http/test_reject_drain failed intermittently on the completion backends, ~15-22% of
# runs under sanitizers, across four different tests whose identity moved between runs. It was not a
# drain bug. Measured, at n=100 per arm:
#
#   target test alone ....................................  0/100
#   after a predecessor that starts no server ............  0/100
#   after 2 / 5 / 11 prior server lifecycles ............. 10 / 13 / 23 per 100
#   11 predecessors + target in ONE process .............. 23/100
#   same 11 first, then target in a FRESH process ........  0/100
#
# Identical work, identical machine; only the process boundary differs. The failure probability rises
# with the number of server lifecycles already run IN THAT PROCESS, and a fresh process resets it. So
# the suite was measuring accumulated process state, not the drain path. One process per test removed
# it completely: 6 failed cycles in 50 for the whole suite in one process, 0 in 50 one-test-per-process
# (900 test executions).
#
# Isolation is applied per suite rather than everywhere because it costs a process launch per test,
# which is real time under sanitizers, and almost every suite is unaffected.
set -e

label=$1; isolated=$2; shift 2

failed=0
for bin in "$@"; do
    name=$(basename "$bin" .exe)
    name=${name#test_}
    iso=0
    for i in $isolated; do
        [ "$i" = "$name" ] && iso=1
    done

    if [ "$iso" -eq 0 ]; then
        echo "--- $bin ---"
        "./$bin" || failed=1
        continue
    fi

    # One process per test. --list-tests prints "suite.test" per line, and those names are exactly what
    # --filter accepts, so this needs no knowledge of the suite contents.
    echo "--- $bin (isolated: one process per test) ---"
    tests=$("./$bin" --list-tests | tr -d "\r" | grep . || true)
    if [ -z "$tests" ]; then
        echo "run_suites: $bin listed no tests; running it whole" >&2
        "./$bin" || failed=1
        continue
    fi
    for t in $tests; do
        "./$bin" --filter="$t" || failed=1
    done
done

if [ "$failed" -eq 1 ]; then
    echo "SOME $label TESTS FAILED"
    exit 1
fi
echo "$label: all suites green"
