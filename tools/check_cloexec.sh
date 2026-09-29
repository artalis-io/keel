#!/bin/sh
# tools/check_cloexec.sh - every descriptor Keel creates is close-on-exec.
#
# An embedder that spawns children must not hand them Keel's own descriptors (event loop, wakeup and
# splice pipes, sockets). tests/test_cloexec.c checks this at run time for everything created at
# setup; this gate covers creation sites the test cannot reach (lazily created descriptors, rare
# paths) by reading the source:
#
#   R1  A creator that TAKES a close-on-exec flag passes it on the same line:
#       pipe2 / epoll_create1 / eventfd / timerfd_create / signalfd / inotify_init1 / memfd_create /
#       accept4 / dup3 need a *CLOEXEC flag (O_CLOEXEC, EPOLL_CLOEXEC, EFD_CLOEXEC, TFD_CLOEXEC,
#       SFD_CLOEXEC, IN_CLOEXEC, MFD_CLOEXEC, SOCK_CLOEXEC).
#   R2  A creator with NO flag (pipe, kqueue, dup, dup2, epoll_create) appears only in the files listed
#       in BARE_ALLOWED, each of which sets FD_CLOEXEC right after creating the descriptor.
#
# Scope: src/ (library code). Comments are blanked first, with newlines kept so line numbers stay
# exact, so prose naming an API ("self-pipe (hosted)", "pipe(2)") cannot trip the gate.
# Self-canaried. Usage: tools/check_cloexec.sh [--selftest]
set -eu

FLAGGED_RE='\b(pipe2|epoll_create1|eventfd|timerfd_create|signalfd|inotify_init1|memfd_create|accept4|dup3)[[:space:]]*\('
CLOEXEC_RE='CLOEXEC'
BARE_RE='\b(pipe|kqueue|dup|dup2|epoll_create)[[:space:]]*\('
BARE_ALLOWED="src/platform_wakeup_posix.c src/event_kqueue.c"

# Blank C comments, keeping one newline per newline inside a block comment.
STRIP='s{/\*(.*?)\*/}{ my $c = $1; " " . ("\n" x ($c =~ tr/\n//)) }gse; s{//[^\n]*}{}g;'
code() { perl -0pe "$STRIP" "$1"; }

selftest() {
    fail=0
    chk() { if printf '%s\n' "$2" | grep -qE "$1"; then g=1; else g=0; fi
            [ "$g" = "$3" ] || { echo "check-cloexec: SELF-TEST FAILED: /$1/ on '$2' gave $g, want $3"; fail=1; }; }
    chk "$FLAGGED_RE" 'if (pipe2(pfd, O_NONBLOCK) < 0)' 1
    chk "$CLOEXEC_RE" 'if (pipe2(pfd, O_NONBLOCK) < 0)' 0
    chk "$CLOEXEC_RE" 'st->fd = epoll_create1(EPOLL_CLOEXEC);' 1
    chk "$BARE_RE" 'if (pipe(fds) < 0)' 1
    chk "$BARE_RE" '    st->fd = kqueue();' 1
    chk "$BARE_RE" 'kl_plat_pipe_close(h); pipe_final(p);' 0
    # The stripper: a commented call must vanish; a real call after a comment must survive; and a
    # multi-line comment must keep its line count.
    t=$(printf '/* self-pipe (hosted)\n spans */ x = kqueue();\n// pipe(2) here\ny = 1;\n' | perl -0pe "$STRIP")
    printf '%s\n' "$t" | grep -qE "$BARE_RE" || { echo "check-cloexec: SELF-TEST FAILED: stripper removed a real call"; fail=1; }
    printf '%s\n' "$t" | grep -qE '\bpipe[[:space:]]*\(' && { echo "check-cloexec: SELF-TEST FAILED: stripper kept a commented call"; fail=1; }
    [ "$(printf '%s\n' "$t" | grep -n 'y = 1' | cut -d: -f1)" = "4" ] || { echo "check-cloexec: SELF-TEST FAILED: stripper shifted line numbers"; fail=1; }
    [ $fail -eq 0 ] || exit 1
}
selftest
[ "${1:-}" = "--selftest" ] && { echo "check-cloexec: selftest OK"; exit 0; }

bad=0
for f in $(git ls-files 'src/*.c' 'src/*.h'); do
    [ -f "$f" ] || continue
    src=$(code "$f")
    hits=$(printf '%s\n' "$src" | grep -nE "$FLAGGED_RE" | grep -v "$CLOEXEC_RE" || true)
    if [ -n "$hits" ]; then
        printf '%s\n' "$hits" | sed "s|^|CLOEXEC VIOLATION (R1 creator without its CLOEXEC flag): $f:|"; bad=1
    fi
    if printf '%s\n' "$src" | grep -qE "$BARE_RE"; then
        case " $BARE_ALLOWED " in
        *" $f "*) printf '%s\n' "$src" | grep -q 'FD_CLOEXEC' || { echo "CLOEXEC VIOLATION (R2 $f creates a flagless descriptor but never sets FD_CLOEXEC)"; bad=1; } ;;
        *) printf '%s\n' "$src" | grep -nE "$BARE_RE" | sed "s|^|CLOEXEC VIOLATION (R2 flagless creator outside the allowlist): $f:|"; bad=1 ;;
        esac
    fi
done
if [ $bad -ne 0 ]; then
    echo "check-cloexec: FAILED (every descriptor Keel creates must be close-on-exec; see tools/check_cloexec.sh)"
    exit 1
fi
echo "check-cloexec: OK (flagged creators pass CLOEXEC; flagless creators only where FD_CLOEXEC follows; self-canary green)"
