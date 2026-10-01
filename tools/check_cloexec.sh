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
#       accept4 / dup3 / io_uring_prep_accept need a *CLOEXEC flag (O_CLOEXEC, EPOLL_CLOEXEC,
#       EFD_CLOEXEC, TFD_CLOEXEC, SFD_CLOEXEC, IN_CLOEXEC, MFD_CLOEXEC, SOCK_CLOEXEC), and WSASocketW /
#       WSASocketA need WSA_FLAG_NO_HANDLE_INHERIT (the Windows analog).
#   R2  A creator with NO flag (pipe, kqueue, dup, dup2, epoll_create, socket, accept) appears only in
#       the files listed in BARE_ALLOWED, each of which marks the descriptor right after creating it
#       (FD_CLOEXEC, or HANDLE_FLAG_INHERIT / kl_sockdef_set_cloexec for a Winsock handle). A vtable
#       call (p->ops->socket(...)) is not a creator.
#   R3  Every file that creates a socket through the provider seam (kl_sock_socket) marks it through the
#       seam too: it calls kl_sock_set_cloexec at least as many times as kl_sock_socket. (A custom
#       provider's socket op need not be close-on-exec at creation, so the caller must ask.)
#   R4  Files too: open / openat pass O_CLOEXEC on the same line, the Windows CRT _open passes
#       _O_NOINHERIT, and stdio's fopen is not used at all (its mode cannot portably ask for it); read a
#       file through kl_plat_fopen_read, which opens it close-on-exec.
#
# Scope: src/ (library code). Comments are blanked first, with newlines kept so line numbers stay
# exact, so prose naming an API ("self-pipe (hosted)", "pipe(2)") cannot trip the gate.
# Self-canaried. Usage: tools/check_cloexec.sh [--selftest]
set -eu

FLAGGED_RE='\b(pipe2|epoll_create1|eventfd|timerfd_create|signalfd|inotify_init1|memfd_create|accept4|dup3|io_uring_prep_accept|WSASocketW|WSASocketA)[[:space:]]*\('
CLOEXEC_RE='CLOEXEC|NO_HANDLE_INHERIT'
BARE_RE='(^|[^>.[:alnum:]_])(pipe|kqueue|dup|dup2|epoll_create|socket|accept)[[:space:]]*\('
BARE_MARK_RE='FD_CLOEXEC|HANDLE_FLAG_INHERIT|kl_sockdef_set_cloexec'
OPEN_RE='(^|[^>.[:alnum:]_])(open|openat)[[:space:]]*\('
WOPEN_RE='(^|[^>.[:alnum:]_])_open[[:space:]]*\('
FOPEN_RE='(^|[^>.[:alnum:]_])(fopen|_wfopen)[[:space:]]*\('
BARE_ALLOWED="src/platform_wakeup_posix.c src/event_kqueue.c src/platform_pipe_posix.c src/socket_posix.c src/socket_winsock.c src/event_pollcomp.c src/platform_wakeup_win.c"

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
    chk "$BARE_RE" '    int a = accept(op->fd, sa, &sl);' 1
    chk "$BARE_RE" 'SOCKET s = socket(domain, type, protocol);' 1
    chk "$BARE_RE" 'return p->ops->socket(p->context, d, t, 0);' 0
    chk "$BARE_RE" 'fd = kl_sock_socket(sockets, family, SOCK_STREAM, 0);' 0
    chk "$BARE_RE" 'c = kl_sockdef_accept(fd, peer);' 0
    chk "$FLAGGED_RE" 'io_uring_prep_accept(sqe, fd, sa, &sl, 0);' 1
    chk "$CLOEXEC_RE" 'io_uring_prep_accept(sqe, fd, sa, &sl, 0);' 0
    chk "$CLOEXEC_RE" 'WSASocketW(f, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);' 1
    chk "$CLOEXEC_RE" 'WSASocketW(f, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);' 0
    chk "$OPEN_RE" '    int fd = open("/dev/urandom", O_RDONLY);' 1
    chk "$OPEN_RE" '    int d = openat(dirfd, name, O_RDONLY | O_CLOEXEC);' 1
    chk "$OPEN_RE" '    h = kl_plat_pipe_open(name);' 0
    chk "$OPEN_RE" '    FILE *f = fdopen(fd, "r");' 0
    chk "$WOPEN_RE" '    int fd = _open(path, _O_RDONLY | _O_NOINHERIT);' 1
    chk "$FOPEN_RE" '    FILE *f = fopen(path, "r");' 1
    chk "$FOPEN_RE" '    FILE *f = kl_plat_fopen_read(path);' 0
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
    hits=$(printf '%s\n' "$src" | grep -nE "$FLAGGED_RE" | grep -vE "$CLOEXEC_RE" || true)
    if [ -n "$hits" ]; then
        printf '%s\n' "$hits" | sed "s|^|CLOEXEC VIOLATION (R1 creator without its CLOEXEC flag): $f:|"; bad=1
    fi
    if printf '%s\n' "$src" | grep -qE "$BARE_RE"; then
        case " $BARE_ALLOWED " in
        *" $f "*) printf '%s\n' "$src" | grep -qE "$BARE_MARK_RE" || { echo "CLOEXEC VIOLATION (R2 $f creates a flagless descriptor but never marks it close-on-exec)"; bad=1; } ;;
        *) printf '%s\n' "$src" | grep -nE "$BARE_RE" | sed "s|^|CLOEXEC VIOLATION (R2 flagless creator outside the allowlist): $f:|"; bad=1 ;;
        esac
    fi
    hits=$(printf '%s\n' "$src" | grep -nE "$OPEN_RE" | grep -v 'O_CLOEXEC' || true)
    if [ -n "$hits" ]; then
        printf '%s\n' "$hits" | sed "s|^|CLOEXEC VIOLATION (R4 open without O_CLOEXEC): $f:|"; bad=1
    fi
    hits=$(printf '%s\n' "$src" | grep -nE "$WOPEN_RE" | grep -v '_O_NOINHERIT' || true)
    if [ -n "$hits" ]; then
        printf '%s\n' "$hits" | sed "s|^|CLOEXEC VIOLATION (R4 _open without _O_NOINHERIT): $f:|"; bad=1
    fi
    hits=$(printf '%s\n' "$src" | grep -nE "$FOPEN_RE" || true)
    if [ -n "$hits" ]; then
        printf '%s\n' "$hits" | sed "s|^|CLOEXEC VIOLATION (R4 fopen; use kl_plat_fopen_read): $f:|"; bad=1
    fi
    [ "$f" = "src/socket.h" ] && continue                # defines the seam wrappers themselves
    n_sock=$(printf '%s\n' "$src" | grep -oE '\bkl_sock_socket[[:space:]]*\(' | wc -l)
    n_mark=$(printf '%s\n' "$src" | grep -oE '\bkl_sock_set_cloexec[[:space:]]*\(' | wc -l)
    if [ "$n_sock" -gt "$n_mark" ]; then
        echo "CLOEXEC VIOLATION (R3 $f: $n_sock kl_sock_socket call(s) but only $n_mark kl_sock_set_cloexec)"; bad=1
    fi
done
if [ $bad -ne 0 ]; then
    echo "check-cloexec: FAILED (every descriptor Keel creates must be close-on-exec; see tools/check_cloexec.sh)"
    exit 1
fi
echo "check-cloexec: OK (flagged creators pass CLOEXEC; flagless creators only where the mark follows; seam sockets marked; file opens close-on-exec; self-canary green)"
