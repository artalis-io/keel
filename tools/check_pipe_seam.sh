#!/bin/sh
# tools/check_pipe_seam.sh - structural gate for Windows Named Pipes (docs/architecture/windows_named_pipes.md §6).
#
# A named pipe is a Win32 HANDLE carried by its own transport (src/pipe_stream.c) over its own seams
# (src/platform_pipe.h, src/completion_pipe.h). This gate keeps it there, default-deny:
#
#   R1  Win32 pipe calls (CreateNamedPipe / ConnectNamedPipe / DisconnectNamedPipe / PeekNamedPipe /
#       SetNamedPipeHandleState / TransactNamedPipe / CallNamedPipe / ImpersonateNamedPipeClient) and
#       ReadFile / WriteFile appear ONLY in the pipe mechanics TUs: src/event_iocp.c (overlapped I/O on
#       the port) and src/platform_pipe_win.c (open/close).
#   R2  No blocking pipe I/O on the loop: WaitNamedPipe appears nowhere, and every ReadFile / WriteFile /
#       ConnectNamedPipe call passes an OVERLAPPED (`&op->ov`).
#   R3  The pipe never enters the socket axis: no pipe symbol (KlPipeHandle, kl_plat_pipe_*,
#       kl_comp_pipe_*, KL_COMP_PIPE_*, KL_IOCP_PIPE_*) in a socket provider / socket seam TU, and no
#       cast of a pipe handle to a socket or integer type in the pipe TUs.
#   R4  Protocol code does not open pipes: nothing under src/protocols/ includes <keel/pipe.h> or a
#       pipe seam header. (The Tier-1 gate separately forbids completion_pipe.h / platform_pipe.h.)
#   R5  The transport stays consumer-neutral: no ssh / agent token, and no stdio-role or protocol token
#       (stdin / stdout / stderr / mcp / jsonrpc), in the pipe transport TUs. Keel makes pipe pairs; which
#       standard stream a pair becomes, and what runs over it, is the embedder's.
#   R6  No process management in Keel: no fork / vfork / exec* / posix_spawn* / waitpid / CreateProcess* /
#       TerminateProcess call anywhere under src/, include/ or integrations/ (comments are stripped first).
#       An anonymous pipe pair is handed to the embedder's spawner; Keel never spawns.
#   R7  POSIX pipe creation stays in its PAL: pipe( / pipe2( calls appear only in
#       src/platform_pipe_posix.c, plus the pre-existing src/platform_wakeup_posix.c (KlWakeup) and
#       src/event_iouring.c (the splice pipe). Comments stripped.
#   R8  No process-global SIGPIPE disposition: signal(SIGPIPE, ...) / sigaction(SIGPIPE, ...) appear only in
#       the one existing HTTP-server site (src/protocols/http/http_server_plat_posix.c). The pipe
#       transport suppresses SIGPIPE per write, per thread, and never changes the disposition.
#   R3 also forbids the pipe transport from calling the socket seam (kl_sock_* / kl_sockdef_*): a POSIX
#       pipe end is a file descriptor, never a socket. Its one registration conversion to KlSocketHandle
#       for the watcher API is kl_plat_pipe_pollable in platform_pipe_posix.c, which is also the one TU
#       exempt from the no-integer-cast rule (on POSIX the native pipe handle IS a descriptor, encoded in
#       the opaque pointer there and nowhere else).
#
# Calls are matched as `Name(` so an explanatory comment naming an API does not trip the gate.
# Usage: tools/check_pipe_seam.sh [--selftest]
set -eu

CALL_RE='\b(CreateNamedPipe[AW]?|ConnectNamedPipe|DisconnectNamedPipe|PeekNamedPipe|SetNamedPipeHandleState|TransactNamedPipe|CallNamedPipe[AW]?|ImpersonateNamedPipeClient|ReadFile|WriteFile)[[:space:]]*\('
WAIT_RE='\bWaitNamedPipe[AW]?[[:space:]]*\('
RW_RE='\b(ReadFile|WriteFile|ConnectNamedPipe)[[:space:]]*\('
OVL_RE='&op->ov'
SYM_RE='KlPipeHandle|kl_plat_pipe_|kl_comp_pipe_|KL_COMP_PIPE_|KL_IOCP_PIPE_'
CAST_RE='\((KlSocketHandle|SOCKET|int|unsigned|long|intptr_t|uintptr_t)\)[[:space:]]*(p->h|h|op->op_handle|pop->h)\b'
INC_RE='#[[:space:]]*include[[:space:]]*[<"](keel/pipe|pipe|completion_pipe|platform_pipe)\.h[>"]'
NEUTRAL_RE='ssh|agent|\b(stdin|stdout|stderr|mcp|jsonrpc)\b'
PROC_RE='\b(fork|vfork|execl|execle|execlp|execv|execve|execvp|execvpe|posix_spawnp?|waitpid|CreateProcess(AsUser)?[AW]?|TerminateProcess)[[:space:]]*\('
PIPE_RE='\b(pipe|pipe2)[[:space:]]*\('
PIPE_ALLOWED="src/platform_pipe_posix.c src/platform_wakeup_posix.c src/event_iouring.c"
SIGPIPE_RE='\b(signal|sigaction)[[:space:]]*\([[:space:]]*SIGPIPE\b'
SIGPIPE_ALLOWED="src/protocols/http/http_server_plat_posix.c"
SOCKSEAM_RE='\bkl_sock(def)?_[a-z_]+[[:space:]]*\('
CAST_EXEMPT="src/platform_pipe_posix.c"
# Blank C comments (keeping newlines, so line numbers stay exact) before the R6 / R7 / R8 scans.
STRIP='s{/\*(.*?)\*/}{ my $c = $1; " " . ("\n" x ($c =~ tr/\n//)) }gse; s{//[^\n]*}{}g;'

MECH="src/event_iocp.c src/platform_pipe_win.c"
PIPE_TUS="src/pipe_stream.c src/platform_pipe.h src/platform_pipe_win.c src/platform_pipe_posix.c src/completion_pipe.h src/completion_pipe_absent.c include/keel/pipe.h include/keel/anon_pipe.h include/keel/anon_pipe_native.h"

selftest() {
    fail=0
    chk() { # $1 regex, $2 flags, $3 sample, $4 expect (1 match / 0 no match)
        if printf '%s\n' "$3" | grep -qE $2 "$1"; then got=1; else got=0; fi
        [ "$got" = "$4" ] || { echo "check-pipe-seam: SELF-TEST FAILED: /$1/ on '$3' gave $got, want $4"; fail=1; }
    }
    chk "$CALL_RE" "" 'x = ConnectNamedPipe(h, &ov);' 1
    chk "$CALL_RE" "" 'ok = ReadFile (h, b, n, NULL, &op->ov);' 1
    chk "$CALL_RE" "" ' * CreateNamedPipeW / ConnectNamedPipe are named in a comment' 0
    chk "$WAIT_RE" "" 'WaitNamedPipeW(name, 1000);' 1
    chk "$OVL_RE" "" 'r = ReadFile(op->op_handle, op->buf, n, NULL, &op->ov);' 1
    chk "$OVL_RE" "" 'r = ReadFile(h, buf, n, &got, NULL);' 0
    chk "$RW_RE" "" 'r = ConnectNamedPipe(h, NULL);' 1
    chk "$SYM_RE" "" 'KlPipeHandle *h;' 1
    chk "$CAST_RE" "" 'KlSocketHandle fd = (KlSocketHandle)p->h;' 1
    chk "$CAST_RE" "" 'CancelIoEx(op->op_handle, &op->ov);' 0
    chk "$INC_RE" "" '#include <keel/pipe.h>' 1
    chk "$INC_RE" "" '#include "completion_pipe.h"' 1
    chk "$INC_RE" "" '#include <keel/stream.h>' 0
    chk "$NEUTRAL_RE" "-i" '/* SSH_AUTH_SOCK */' 1
    chk "$NEUTRAL_RE" "-i" '/* hand B to the child as its STDIN */' 1
    chk "$NEUTRAL_RE" "-i" 'an MCP server over the pair' 1
    chk "$NEUTRAL_RE" "-i" 'kl_comp_pipe_post(ctx, &op);' 0
    chk "$PROC_RE" "" 'pid_t pid = fork();' 1
    chk "$PROC_RE" "" 'ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);' 1
    chk "$PROC_RE" "" 'if (posix_spawnp(&pid, argv[0], NULL, NULL, argv, envp) != 0)' 1
    chk "$PROC_RE" "" 'kl_platform_forkless_init();' 0
    chk "$PIPE_RE" "" 'if (pipe2(fds, O_CLOEXEC) < 0)' 1
    chk "$PIPE_RE" "" 'if (pipe(fds) < 0)' 1
    chk "$PIPE_RE" "" 'kl_plat_pipe_close(h);' 0
    chk "$SIGPIPE_RE" "" 'signal(SIGPIPE, SIG_IGN);' 1
    chk "$SIGPIPE_RE" "" 'sigaction(SIGPIPE, &sa, NULL);' 1
    chk "$SIGPIPE_RE" "" 'sigaddset(&pipe_only, SIGPIPE);' 0
    chk "$SOCKSEAM_RE" "" 'n = kl_sockdef_send(fd, b, n);' 1
    chk "$SOCKSEAM_RE" "" 'n = kl_sock_recv(prov, fd, b, n);' 1
    chk "$SOCKSEAM_RE" "" 'n = kl_plat_pipe_write(p->h, d, n, &wb);' 0
    t=$(printf '/* not inherited across fork(), mark it */\nx = 1; // exec(\ny = waitpid(p, &s, 0);\n' | perl -0pe "$STRIP")
    printf '%s\n' "$t" | grep -nE "$PROC_RE" | grep -q '^3:' || { echo "check-pipe-seam: SELF-TEST FAILED: stripper lost a real call or shifted lines"; fail=1; }
    [ "$(printf '%s\n' "$t" | grep -cE "$PROC_RE")" = 1 ] || { echo "check-pipe-seam: SELF-TEST FAILED: stripper kept a commented call"; fail=1; }
    [ $fail -eq 0 ] || exit 1
}

selftest
[ "${1:-}" = "--selftest" ] && { echo "check-pipe-seam: selftest OK"; exit 0; }

bad=0
report() { echo "PIPE-SEAM VIOLATION ($1): $2"; bad=1; }

for f in $(git ls-files 'src/*.c' 'src/*.h' 'src/**/*.c' 'src/**/*.h' 'include/keel/*.h' 'integrations/**/*.c' 'integrations/**/*.h' | sort -u); do
    [ -f "$f" ] || continue
    case " $MECH " in
    *" $f "*) ;;
    *) grep -nE "$CALL_RE" "$f" | while IFS= read -r l; do echo "PIPE-SEAM VIOLATION (R1 pipe/file I/O outside the mechanics TUs): $f:$l"; done ;;
    esac
    grep -nE "$WAIT_RE" "$f" | while IFS= read -r l; do echo "PIPE-SEAM VIOLATION (R2 blocking WaitNamedPipe): $f:$l"; done
done > "${TMPDIR:-/tmp}/pipe_seam.$$" 2>&1 || true
if [ -s "${TMPDIR:-/tmp}/pipe_seam.$$" ]; then cat "${TMPDIR:-/tmp}/pipe_seam.$$"; bad=1; fi
rm -f "${TMPDIR:-/tmp}/pipe_seam.$$"

# R2: every ReadFile / WriteFile in the mechanics TUs is overlapped.
for f in $MECH; do
    grep -nE "$RW_RE" "$f" | grep -vF "$OVL_RE" | while IFS= read -r l; do
        echo "PIPE-SEAM VIOLATION (R2 non-overlapped ReadFile/WriteFile/ConnectNamedPipe): $f:$l"
    done
done | grep . && bad=1

# R3: no pipe symbol on the socket axis; no pipe-handle narrowing casts.
for f in $(git ls-files 'src/socket*.c' 'src/socket*.h' 'src/platform_socket*' 'src/sockcompat.h' 'src/sockaddr*' \
                         'include/keel/socket.h' 'include/keel/handle.h' 'include/keel/net.h'); do
    grep -nE "$SYM_RE" "$f" | while IFS= read -r l; do echo "PIPE-SEAM VIOLATION (R3 pipe symbol on the socket axis): $f:$l"; done
done | grep . && bad=1
for f in $PIPE_TUS src/event_iocp.c; do
    [ -f "$f" ] || continue
    case " $CAST_EXEMPT " in *" $f "*) continue ;; esac
    grep -nE "$CAST_RE" "$f" | while IFS= read -r l; do echo "PIPE-SEAM VIOLATION (R3 pipe handle cast to a socket/integer type): $f:$l"; done
done | grep . && bad=1
for f in $PIPE_TUS; do
    [ -f "$f" ] || continue
    perl -0pe "$STRIP" "$f" | grep -nE "$SOCKSEAM_RE" | while IFS= read -r l; do
        echo "PIPE-SEAM VIOLATION (R3 pipe transport calls the socket seam): $f:$l"
    done
done | grep . && bad=1

# R4: protocol TUs do not reach the pipe transport.
for f in $(git ls-files 'src/protocols/*'); do
    grep -nE "$INC_RE" "$f" | while IFS= read -r l; do echo "PIPE-SEAM VIOLATION (R4 protocol TU includes a pipe header): $f:$l"; done
done | grep . && bad=1

# R5: the transport knows no consumer protocol.
for f in $PIPE_TUS; do
    [ -f "$f" ] || continue
    grep -niE "$NEUTRAL_RE" "$f" | while IFS= read -r l; do echo "PIPE-SEAM VIOLATION (R5 consumer-specific token in the pipe transport): $f:$l"; done
done | grep . && bad=1

# R7 / R8: POSIX pipe creation and SIGPIPE disposition stay where they belong.
for f in $(git ls-files 'src/*.c' 'src/*.h' 'include/*.h' 'integrations/*.c' 'integrations/*.h'); do
    [ -f "$f" ] || continue
    src=$(perl -0pe "$STRIP" "$f")
    case " $PIPE_ALLOWED " in
    *" $f "*) ;;
    *) printf '%s\n' "$src" | grep -nE "$PIPE_RE" | while IFS= read -r l; do echo "PIPE-SEAM VIOLATION (R7 pipe creation outside the pipe PAL): $f:$l"; done ;;
    esac
    case " $SIGPIPE_ALLOWED " in
    *" $f "*) ;;
    *) printf '%s\n' "$src" | grep -nE "$SIGPIPE_RE" | while IFS= read -r l; do echo "PIPE-SEAM VIOLATION (R8 process-global SIGPIPE disposition): $f:$l"; done ;;
    esac
done | grep . && bad=1

# R6: Keel never manages processes.
for f in $(git ls-files 'src/*.c' 'src/*.h' 'include/*.h' 'integrations/*.c' 'integrations/*.h'); do
    [ -f "$f" ] || continue
    perl -0pe "$STRIP" "$f" | grep -nE "$PROC_RE" | while IFS= read -r l; do
        echo "PIPE-SEAM VIOLATION (R6 process management in Keel): $f:$l"
    done
done | grep . && bad=1

if [ $bad -ne 0 ]; then
    echo "check-pipe-seam: FAILED (see docs/architecture/windows_named_pipes.md §6)"
    exit 1
fi
echo "check-pipe-seam: OK (pipe I/O confined to event_iocp.c + platform_pipe_win.c, overlapped only; no pipe symbol on the socket axis; no protocol reaches it; consumer-neutral; no process management; POSIX pipe creation in its PAL; no SIGPIPE disposition change; self-canary green)"
