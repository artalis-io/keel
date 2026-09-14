#!/bin/sh
# check_msvc_parity.sh: the MSVC suite set is DERIVED, and stays that way.
#
# The contract this enforces:
#
#     MSVC eligible set  =  Windows semantic suite set  -  documented exclusions
#
# so a new Windows suite is enrolled in MSVC coverage by DEFAULT, and keeping one out is
# a deliberate, documented act rather than an omission. Backend-aware: WSAPoll (readiness)
# and IOCP (completion) have different base sets, and the contract must hold for both.
#
# The Makefile owns the derivation; this script only checks it, with the sets passed in as
# environment so there is exactly one source of truth. POSIX sh only: CI runs /bin/sh as
# dash, so no process substitution and no bash arrays. Self-canaried via --selftest.
set -u

MAKEFILE=${MAKEFILE:-Makefile}
fail() { echo "check-msvc-parity: FAIL - $1" >&2; exit 1; }
norm() { printf '%s' "$1" | tr ' \t' '\n\n' | sed '/^$/d' | LC_ALL=C sort -u; }

TMPD=$(mktemp -d) || exit 1
trap 'rm -rf "$TMPD"' EXIT INT TERM
minus() { LC_ALL=C comm -23 "$1" "$2"; }        # A \ B, both sorted
blank() { [ -z "$(printf '%s' "$1" | tr -d ' \n\t')" ]; }

check_backend() {
    label=$1
    norm "$2" > "$TMPD/b"; norm "$3" > "$TMPD/e"; norm "$4" > "$TMPD/x"

    # 1. Enrolled is exactly base minus exclusions. Catches a manual MSVC_TEST_SUITES
    #    override (it is ?=, so the environment can displace it) and any drift.
    minus "$TMPD/b" "$TMPD/x" > "$TMPD/want"
    if ! cmp -s "$TMPD/e" "$TMPD/want"; then
        echo "  enrolled but should not be: $(minus "$TMPD/e" "$TMPD/want" | tr '\n' ' ')" >&2
        echo "  missing from the MSVC set:  $(minus "$TMPD/want" "$TMPD/e" | tr '\n' ' ')" >&2
        fail "$label: the MSVC set is not (base - exclusions); the derivation was bypassed"
    fi

    # 2. Nothing enrolled that this backend does not even have.
    stray=$(minus "$TMPD/e" "$TMPD/b" | tr '\n' ' ')
    blank "$stray" || fail "$label: enrolled suites absent from the Windows set: $stray"
}

# 3. An exclusion naming a suite that exists in neither base set is stale: the suite was
#    renamed or deleted and the exclusion outlived it, silently shrinking the contract.
check_no_stale() {
    norm "$1 $2" > "$TMPD/all"; norm "$3" > "$TMPD/xa"
    stale=$(minus "$TMPD/xa" "$TMPD/all" | tr '\n' ' ')
    blank "$stale" || fail "stale exclusion(s) for suites that no longer exist: $stale"
}

# 4. Every non-empty exclusion list carries its reason in the Makefile: a contiguous comment
#    block immediately above it, long enough to be an explanation and not a label.
check_documented() {
    for var in MSVC_EXCLUDE_PTHREAD MSVC_EXCLUDE_UCRT MSVC_EXCLUDE_ICE; do
        eval "val=\${$var:-}"
        blank "$val" && continue
        ln=$(grep -n "^$var *=" "$MAKEFILE" 2>/dev/null | head -1 | cut -d: -f1)
        [ -n "$ln" ] || fail "$var excludes suites but is not defined in $MAKEFILE"
        chars=0; lines=0; i=$((ln - 1))
        while [ "$i" -gt 0 ]; do
            t=$(sed -n "${i}p" "$MAKEFILE")
            case "$t" in
                '#'*) chars=$((chars + ${#t})); lines=$((lines + 1)); i=$((i - 1)) ;;
                *) break ;;
            esac
        done
        if [ "$lines" -lt 2 ] || [ "$chars" -lt 80 ]; then
            fail "$var excludes suites with no documented reason above it in $MAKEFILE (${lines} comment line(s), ${chars} chars)"
        fi
    done
}

if [ "${1:-}" = "--selftest" ]; then
    caught=0
    probe() {
        d=$1; shift
        if ( "$@" ) >/dev/null 2>&1; then
            echo "check-msvc-parity: SELFTEST FAILED - not detected: $d" >&2; exit 1
        fi
        echo "  canary caught: $d"; caught=$((caught + 1))
    }
    E='MSVC_EXCLUDE_PTHREAD= MSVC_EXCLUDE_UCRT= MSVC_EXCLUDE_ICE='
    probe "a suite dropped from the MSVC set without an exclusion" \
        env LABEL=t BASE_CUR="a b c" ENROLLED_CUR="a b" BASE_WSAPOLL="a b c" BASE_IOCP="a b c" \
            EXCL_ALL="" $E MAKEFILE="$MAKEFILE" sh "$0"
    probe "a suite enrolled that the backend does not have" \
        env LABEL=t BASE_CUR="a b" ENROLLED_CUR="a b zz" BASE_WSAPOLL="a b" BASE_IOCP="a b" \
            EXCL_ALL="" $E MAKEFILE="$MAKEFILE" sh "$0"
    probe "an exclusion for a suite that no longer exists" \
        env LABEL=t BASE_CUR="a b" ENROLLED_CUR="a b" BASE_WSAPOLL="a b" BASE_IOCP="a b" \
            EXCL_ALL="ghost" $E MAKEFILE="$MAKEFILE" sh "$0"
    probe "an exclusion list with no documented reason" \
        env LABEL=t BASE_CUR="a b" ENROLLED_CUR="a" BASE_WSAPOLL="a b" BASE_IOCP="a b" \
            EXCL_ALL="b" MSVC_EXCLUDE_PTHREAD= MSVC_EXCLUDE_UCRT="b" MSVC_EXCLUDE_ICE= \
            MAKEFILE=/dev/null sh "$0"
    probe "the IOCP base set drifting from its enrolled set" \
        env LABEL=iocp BASE_CUR="a b c" ENROLLED_CUR="a b" BASE_WSAPOLL="a b" BASE_IOCP="a b c" \
            EXCL_ALL="" $E MAKEFILE="$MAKEFILE" sh "$0"
    echo "check-msvc-parity selftest: OK ($caught canaries detected)"
    exit 0
fi

# ENROLLED_CUR is the REAL $(MSVC_TEST_SUITES) for the backend being built, not a
# recomputation of the derivation: that is what makes a manual override visible here.
check_backend "$LABEL" "$BASE_CUR" "$ENROLLED_CUR" "$EXCL_ALL"
check_no_stale "$BASE_WSAPOLL" "$BASE_IOCP" "$EXCL_ALL"
check_documented

n=$(norm "$ENROLLED_CUR" | wc -l); b=$(norm "$BASE_CUR" | wc -l)
nx=$(norm "$EXCL_ALL" | wc -l)
echo "check-msvc-parity[$LABEL]: OK ($n of $b enrolled; $nx documented exclusion(s))"
