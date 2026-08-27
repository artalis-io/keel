#!/bin/sh
# tools/check_no_eventloop_fd.sh - F2-D resurrection gate: KlEventLoop.fd stays removed.
#
# Default-deny, self-canaried, file:line diagnostics, BSD+GNU-portable, run over tracked files.
# Rejects two resurrections:
#   (1) an `fd` member reappearing in the public KlEventLoop struct (include/keel/event.h);
#   (2) any read/write of a KlEventLoop's fd (loop->fd / loop.fd / .loop.fd) in tracked C/H sources,
#       which also blocks a new generic or protocol consumer from reaching for it.
# The epoll/kqueue descriptor now lives in backend-owned private state (loop->_backend), so ZERO
# occurrences are permitted. Living docs (*.md) legitimately discuss the removed field and are outside
# the scan (it covers *.c/*.h only). Not enrolled in remote CI until F2-6.
set -eu

ROOT=$(git rev-parse --show-toplevel 2>/dev/null || echo .)
cd "$ROOT"
fail=0

# Access detector (word-boundary emulation so g_loop.fd / st->fd do not false-positive).
ACCESS_RE='(^|[^A-Za-z0-9_])loop->fd([^A-Za-z0-9_]|$)|(^|[^A-Za-z0-9_])loop\.fd([^A-Za-z0-9_]|$)|\.loop\.fd([^A-Za-z0-9_]|$)'

# --- self-canary: the detector must fire on a resurrection and stay silent on a lookalike ---------
det() { printf '%s\n' "$1" | grep -Eq "$ACCESS_RE"; }
det 'x = ctx->loop.fd;'          || { echo "check-no-eventloop-fd: SELF-TEST failed (missed .loop.fd)"; exit 1; }
det 'close(loop->fd);'           || { echo "check-no-eventloop-fd: SELF-TEST failed (missed loop->fd)"; exit 1; }
if det 'g_loop.watches[i].fd = 3;'; then echo "check-no-eventloop-fd: SELF-TEST failed (false positive g_loop)"; exit 1; fi
if det 'st->fd = kqueue();';       then echo "check-no-eventloop-fd: SELF-TEST failed (false positive st->fd)"; exit 1; fi

# --- case 1: no `fd` member in the KlEventLoop struct --------------------------------------------
hdr=include/keel/event.h
body=$(awk '/typedef struct \{/{inb=1} inb{print} /\} KlEventLoop;/{exit}' "$hdr" | sed -E 's@//.*@@')
if printf '%s\n' "$body" | grep -Eq '(^|[^A-Za-z0-9_])fd[[:space:]]*(;|\[)'; then
    echo "check-no-eventloop-fd: $hdr: KlEventLoop must not declare an 'fd' member (F2-D removed it; use _backend)"
    fail=1
fi

# --- case 2: no loop->fd / loop.fd / .loop.fd access in tracked C/H ------------------------------
gate='tools/check_no_eventloop_fd.sh'
hits=$(git ls-files '*.c' '*.h' | while IFS= read -r f; do
    [ "$f" = "$gate" ] && continue
    sed -E 's@//.*@@' "$f" | grep -nE "$ACCESS_RE" | sed "s@^@$f:@"
done)
if [ -n "$hits" ]; then
    echo "check-no-eventloop-fd: a KlEventLoop fd access resurfaced (keep the descriptor in loop->_backend):"
    printf '%s\n' "$hits" | sed 's/^/  /'
    fail=1
fi

[ "$fail" = 0 ] && echo "check-no-eventloop-fd: OK (KlEventLoop.fd stays removed; the descriptor is backend-owned)"
exit "$fail"
