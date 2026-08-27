#!/bin/sh
# tools/f2_opaque_probe.sh - F2 opaque-type installed-consumer probe.
#
# Stage-installs the headers, then proves from an out-of-tree consumer that a FIELD DEREFERENCE on an
# opaque public type fails to compile, while using it as a pointer compiles. Self-canaried (the
# positive controls guard against the type accidentally becoming concrete). Covers the substrate
# opaque types KlWatcher and KlTimerEntry. C11 and, where available, C++11.
set -eu

ROOT=$(git rev-parse --show-toplevel 2>/dev/null || echo .)
cd "$ROOT"
CC=${CC:-cc}
CXX=${CXX:-c++}

stage=$(mktemp -d)
tmp=$(mktemp -d)
trap 'rm -rf "$stage" "$tmp"; rm -f "$ROOT/keel.pc"' EXIT INT TERM

rm -f "$ROOT/keel.pc"                       # force keel.pc regeneration for the staged prefix
make -s install PREFIX="$stage" >/dev/null 2>&1
INC="$stage/include"
[ -f "$INC/keel/event_ctx.h" ] || { echo "opaque-probe: staged headers missing"; exit 2; }

bad=0
# a field-deref consumer per opaque type (must FAIL) and a pointer-use consumer (must PASS).
printf '#include <keel/event_ctx.h>\nint f(KlWatcher *w){ return (int)w->fd; }\n'            > "$tmp/w_deref.c"
printf '#include <keel/timer.h>\nlong f(KlTimerEntry *t){ return (long)t->deadline_ms; }\n'  > "$tmp/t_deref.c"
printf '#include <keel/event_ctx.h>\nKlWatcher *f(KlWatcher *w){ return w; }\n'              > "$tmp/w_ptr.c"
printf '#include <keel/timer.h>\nKlTimerEntry *f(KlTimerEntry *t){ return t; }\n'            > "$tmp/t_ptr.c"
# KlHttpConn: a borrowed handle. Consumers may hold/pass KlHttpConn* but must not deref, sizeof, or
# instantiate it.
printf '#include <keel/http_connection.h>\nint  f(KlHttpConn *c){ return (int)c->state; }\n'     > "$tmp/hc_deref.c"
printf '#include <keel/http_connection.h>\n#include <stddef.h>\nsize_t f(void){ return sizeof(KlHttpConn); }\n' > "$tmp/hc_sizeof.c"
printf '#include <keel/http_connection.h>\nvoid f(void){ KlHttpConn c; (void)c; }\n'             > "$tmp/hc_inst.c"
printf '#include <keel/http_connection.h>\nKlHttpConn *f(KlHttpConn *c){ return c; }\n'          > "$tmp/hc_ptr.c"

must_fail() {  # label file
    if (cd "$tmp" && $CC -std=c11 -I"$INC" -fsyntax-only "$2") 2>/dev/null; then
        echo "opaque-probe: $1 COMPILED but must fail (type is not opaque)"; bad=1
    else
        echo "opaque-probe: $1 correctly rejected (field deref on an opaque type fails)"
    fi
}
must_pass() {  # label file
    if (cd "$tmp" && $CC -std=c11 -I"$INC" -fsyntax-only "$2") 2>/dev/null; then
        echo "opaque-probe: $1 compiles (pointer use, no layout needed)"
    else
        echo "opaque-probe: $1 FAILED to compile (positive control broke)"; bad=1
    fi
}

must_fail "KlWatcher.fd deref"          "$tmp/w_deref.c"
must_fail "KlTimerEntry.deadline_ms"    "$tmp/t_deref.c"
must_pass "KlWatcher pointer use"       "$tmp/w_ptr.c"
must_pass "KlTimerEntry pointer use"    "$tmp/t_ptr.c"
must_fail "KlHttpConn field deref"      "$tmp/hc_deref.c"
must_fail "KlHttpConn sizeof"           "$tmp/hc_sizeof.c"
must_fail "KlHttpConn instantiate"      "$tmp/hc_inst.c"
must_pass "KlHttpConn pointer use"      "$tmp/hc_ptr.c"

# C++ mirror of the negative controls, where a C++ compiler exists.
if command -v "$CXX" >/dev/null 2>&1; then
    for n in w_deref t_deref hc_deref hc_sizeof hc_inst; do cp "$tmp/$n.c" "$tmp/$n.cpp"; done
    for n in w_deref t_deref hc_deref hc_sizeof hc_inst; do
        if (cd "$tmp" && $CXX -std=c++11 -I"$INC" -fsyntax-only "$n.cpp") 2>/dev/null; then
            echo "opaque-probe: C++ $n COMPILED but must fail"; bad=1
        else echo "opaque-probe: C++ $n correctly rejected"; fi
    done
    # C++ positive: holding/passing the opaque handle pointer must compile.
    cp "$tmp/hc_ptr.c" "$tmp/hc_ptr.cpp"
    if (cd "$tmp" && $CXX -std=c++11 -I"$INC" -fsyntax-only hc_ptr.cpp) 2>/dev/null; then
        echo "opaque-probe: C++ KlHttpConn pointer use compiles"
    else echo "opaque-probe: C++ KlHttpConn pointer use FAILED (positive control broke)"; bad=1; fi
fi

[ "$bad" = 0 ] && echo "opaque-probe: OK (KlWatcher, KlTimerEntry, KlHttpConn are opaque on the installed surface)"
exit "$bad"
