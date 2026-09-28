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
#   R2  No blocking pipe I/O on the loop: WaitNamedPipe appears nowhere, and every ReadFile / WriteFile
#       call passes an OVERLAPPED (`&op->ov`).
#   R3  The pipe never enters the socket axis: no pipe symbol (KlPipeHandle, kl_plat_pipe_*,
#       kl_comp_pipe_*, KL_COMP_PIPE_*, KL_IOCP_PIPE_*) in a socket provider / socket seam TU, and no
#       cast of a pipe handle to a socket or integer type in the pipe TUs.
#   R4  Protocol code does not open pipes: nothing under src/protocols/ includes <keel/pipe.h> or a
#       pipe seam header. (The Tier-1 gate separately forbids completion_pipe.h / platform_pipe.h.)
#   R5  The transport stays consumer-neutral: no ssh / agent token in the pipe transport TUs.
#
# Calls are matched as `Name(` so an explanatory comment naming an API does not trip the gate.
# Usage: tools/check_pipe_seam.sh [--selftest]
set -eu

CALL_RE='\b(CreateNamedPipe[AW]?|ConnectNamedPipe|DisconnectNamedPipe|PeekNamedPipe|SetNamedPipeHandleState|TransactNamedPipe|CallNamedPipe[AW]?|ImpersonateNamedPipeClient|ReadFile|WriteFile)[[:space:]]*\('
WAIT_RE='\bWaitNamedPipe[AW]?[[:space:]]*\('
RW_RE='\b(ReadFile|WriteFile)[[:space:]]*\('
OVL_RE='&op->ov'
SYM_RE='KlPipeHandle|kl_plat_pipe_|kl_comp_pipe_|KL_COMP_PIPE_|KL_IOCP_PIPE_'
CAST_RE='\((KlSocketHandle|SOCKET|int|unsigned|long|intptr_t|uintptr_t)\)[[:space:]]*(p->h|h|op->op_handle|pop->h)\b'
INC_RE='#[[:space:]]*include[[:space:]]*[<"](keel/pipe|pipe|completion_pipe|platform_pipe)\.h[>"]'
NEUTRAL_RE='ssh|agent'

MECH="src/event_iocp.c src/platform_pipe_win.c"
PIPE_TUS="src/pipe_stream.c src/platform_pipe.h src/platform_pipe_win.c src/platform_pipe_posix.c src/completion_pipe.h src/completion_pipe_absent.c include/keel/pipe.h"

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
    chk "$SYM_RE" "" 'KlPipeHandle *h;' 1
    chk "$CAST_RE" "" 'KlSocketHandle fd = (KlSocketHandle)p->h;' 1
    chk "$CAST_RE" "" 'CancelIoEx(op->op_handle, &op->ov);' 0
    chk "$INC_RE" "" '#include <keel/pipe.h>' 1
    chk "$INC_RE" "" '#include "completion_pipe.h"' 1
    chk "$INC_RE" "" '#include <keel/stream.h>' 0
    chk "$NEUTRAL_RE" "-i" '/* SSH_AUTH_SOCK */' 1
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
        echo "PIPE-SEAM VIOLATION (R2 non-overlapped ReadFile/WriteFile): $f:$l"
    done
done | grep . && bad=1

# R3: no pipe symbol on the socket axis; no pipe-handle narrowing casts.
for f in $(git ls-files 'src/socket*.c' 'src/socket*.h' 'src/platform_socket*' 'src/sockcompat.h' 'src/sockaddr*' \
                         'include/keel/socket.h' 'include/keel/handle.h' 'include/keel/net.h'); do
    grep -nE "$SYM_RE" "$f" | while IFS= read -r l; do echo "PIPE-SEAM VIOLATION (R3 pipe symbol on the socket axis): $f:$l"; done
done | grep . && bad=1
for f in $PIPE_TUS src/event_iocp.c; do
    [ -f "$f" ] || continue
    grep -nE "$CAST_RE" "$f" | while IFS= read -r l; do echo "PIPE-SEAM VIOLATION (R3 pipe handle cast to a socket/integer type): $f:$l"; done
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

if [ $bad -ne 0 ]; then
    echo "check-pipe-seam: FAILED (see docs/architecture/windows_named_pipes.md §6)"
    exit 1
fi
echo "check-pipe-seam: OK (pipe I/O confined to event_iocp.c + platform_pipe_win.c, overlapped only; no pipe symbol on the socket axis; no protocol reaches it; consumer-neutral; self-canary green)"
