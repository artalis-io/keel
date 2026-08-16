#!/usr/bin/env bash
# run_dgram_public.sh — 7B-9 harness: the PUBLIC KlDatagram close over EFI_UDP4 on real
# firmware. One QEMU/OVMF boot of one EFI image. The guest does a public-API datagram
# round-trip to a host UDP echo (over SLIRP 10.0.2.2) and then a graceful DETACHED close.
#
#   Assert (oracle): terminal DONE reached; GO; send accepted; the echo was delivered back
#   to the armed recv (recv_matched); close settled DETACHED (close_state=CLOSED,
#   close_result=DETACHED); udp_live==0 and udp_quarantined==0 (no leaked/quarantined
#   EFI_UDP4 child). The host echo log must show the guest's probe arriving.
#
# The self-test reports FACTS; THIS harness is the oracle (exit non-zero unless the boot
# passes). Runs inside a container/host that has qemu-system-x86_64 + OVMF + clang + lld +
# mtools + python3. The guest reaches host services at 10.0.2.2 (the SLIRP host/gateway).
#
# Env:
#   KEEL_ROOT       repo root (default: ../..)
#   KL_U9_ECHO_PORT host UDP echo port (default 17777)
#   OVMF_CODE/VARS  override OVMF fds
#   BOOT_TIMEOUT    qemu safety timeout seconds (default 120)
set -uo pipefail
cd "$(dirname "$0")"

: "${KEEL_ROOT:=../..}"
KL_U9_ECHO_PORT="${KL_U9_ECHO_PORT:-17777}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-120}"
export KL_U9_ECHO_PORT

echo "=== 7b9 run harness (public KlDatagram over EFI_UDP4: round-trip + DETACHED close) ==="
echo "echo_port=$KL_U9_ECHO_PORT"

# ---- 1. build the self-contained datagram archive (x86_64) ----
pick() { for c in "$@"; do command -v "$c" >/dev/null 2>&1 && { echo "$c"; return; }; done; }
LLVM_NM="$(pick llvm-nm llvm-nm-18 llvm-nm-17 llvm-nm-16)"
LLVM_AR="$(pick llvm-ar llvm-ar-18 llvm-ar-17 llvm-ar-16)"
ARCHIVE_ARGS=()
[ -n "$LLVM_NM" ] && ARCHIVE_ARGS+=( "NM=$LLVM_NM" )
[ -n "$LLVM_AR" ] && ARCHIVE_ARGS+=( "AR=$LLVM_AR" )
echo "building datagram archive (${ARCHIVE_ARGS[*]:-default tools}) ..."
( cd "$KEEL_ROOT" && make freestanding-lib-dgram-selfcontained "${ARCHIVE_ARGS[@]}" ) \
  || { echo "DATAGRAM ARCHIVE BUILD FAILED"; exit 2; }

# ---- 2. build the EFI app ----
KL_U9_ECHO_PORT="$KL_U9_ECHO_PORT" ./build_dgram_public.sh || { echo "BUILD FAILED"; exit 2; }

# ---- 3. ESP FAT image ----
ESP=esp_pub.img
rm -f "$ESP"
dd if=/dev/zero of="$ESP" bs=1M count=48 status=none
mformat -i "$ESP" -F ::
mmd    -i "$ESP" ::/EFI
mmd    -i "$ESP" ::/EFI/BOOT
mcopy  -i "$ESP" BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
mcopy  -i "$ESP" startup.nsh ::/startup.nsh
echo "ESP image built:"; mdir -i "$ESP" ::/EFI/BOOT/

# ---- 4. host UDP echo responder on 0.0.0.0:$KL_U9_ECHO_PORT (SLIRP -> guest) ----
cat > /tmp/u9_echo.py <<'PYEOF'
import socket, sys, os
port = int(os.environ.get("KL_U9_ECHO_PORT", "17777"))
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", port))
sys.stderr.write("7b9 UDP echo up on :%d\n" % port); sys.stderr.flush()
while True:
    try:
        data, addr = s.recvfrom(1500)
    except Exception:
        continue
    sys.stderr.write("7b9 echo recv %d bytes %r from %s\n" % (len(data), data[:24], addr))
    sys.stderr.flush()
    try:
        s.sendto(data, addr)
    except Exception as e:
        sys.stderr.write("7b9 echo sendto FAILED: %s\n" % e); sys.stderr.flush()
PYEOF
KL_U9_ECHO_PORT="$KL_U9_ECHO_PORT" python3 /tmp/u9_echo.py >/tmp/u9_echo.log 2>&1 &
ECHO_PID=$!
sleep 1
echo "7b9 UDP echo pid=$ECHO_PID on 0.0.0.0:$KL_U9_ECHO_PORT"
cleanup() { kill "$ECHO_PID" 2>/dev/null || true; }
trap cleanup EXIT

# ---- 5. locate OVMF ----
find_ovmf() { for c in "${OVMF_CODE:-}" /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/ovmf/OVMF.fd; do [ -n "$c" ] && [ -f "$c" ] && { echo "$c"; return; }; done; }
find_vars() { for v in "${OVMF_VARS:-}" /usr/share/OVMF/OVMF_VARS_4M.fd /usr/share/OVMF/OVMF_VARS.fd; do [ -n "$v" ] && [ -f "$v" ] && { echo "$v"; return; }; done; }
OVMF_CODE_FD="$(find_ovmf)"; OVMF_VARS_FD="$(find_vars)"
if [ -z "$OVMF_CODE_FD" ]; then echo "ERROR: no OVMF code fd found"; exit 3; fi
echo "OVMF code: $OVMF_CODE_FD ; vars: ${OVMF_VARS_FD:-<none>}"

# ---- 6. boot ----
SERIAL=/tmp/serial_pub.log
VARS_RW=vars_pub.fd
[ -n "$OVMF_VARS_FD" ] && cp "$OVMF_VARS_FD" "$VARS_RW"
args=( -machine q35 -m 512 -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE_FD" )
[ -n "$OVMF_VARS_FD" ] && args+=( -drive if=pflash,format=raw,file="$VARS_RW" )
args+=( -drive format=raw,file="$ESP" -netdev user,id=n0 -device e1000,netdev=n0
        -serial stdio -display none -monitor none -no-reboot )
echo "=== boot (timeout ${BOOT_TIMEOUT}s) ==="
timeout "${BOOT_TIMEOUT}" qemu-system-x86_64 "${args[@]}" 2>&1 | tee "$SERIAL"
QEMU_RC=${PIPESTATUS[0]}   # capture BEFORE any other command resets PIPESTATUS
echo "(qemu exit: $QEMU_RC — 0=clean guest power-off via ResetSystem; 124=safety-timeout; other=crash/qemu error)"

echo "=== host UDP echo log ==="; cat /tmp/u9_echo.log || true

# ---- 7. oracle ----
has() { grep -qE "$2" "$1"; }
ok=1
echo "--- 7b9 oracle ---"
has "$SERIAL" '7b9: DONE'                      && echo "  [ok] terminal DONE reached"            || { echo "  [FAIL] no DONE (crash/hang)"; ok=0; }
has "$SERIAL" '7b9: GO'                          && echo "  [ok] GO"                               || { echo "  [FAIL] no GO"; ok=0; }
has "$SERIAL" '7b9: send = 0'                    && echo "  [ok] send accepted (KL_DATAGRAM_ACCEPTED)" || { echo "  [FAIL] send not accepted"; ok=0; }
has "$SERIAL" '7b9: recv_matched = 1'            && echo "  [ok] echo delivered to armed recv (round-trip)" || { echo "  [FAIL] no matching echo delivered"; ok=0; }
has "$SERIAL" '7b9: close_state = 2'             && echo "  [ok] close reached CLOSED"             || { echo "  [FAIL] close not CLOSED"; ok=0; }
has "$SERIAL" '7b9: close_result = 1'            && echo "  [ok] close_result = DETACHED"          || { echo "  [FAIL] close_result != DETACHED"; ok=0; }
has "$SERIAL" '7b9: udp_live = 0 udp_quarantined = 0' && echo "  [ok] udp_live=0 quarantined=0 (clean teardown)" || { echo "  [FAIL] dirty UDP teardown"; ok=0; }
has /tmp/u9_echo.log 'echo recv'                 && echo "  [ok] host echo saw the guest probe"    || { echo "  [FAIL] host echo never saw the probe"; ok=0; }
# The markers above are all emitted BEFORE the app returns/ResetSystem, so a post-DONE crash,
# failed reset, or hang would still print them. Require QEMU to have exited CLEANLY (0) — a
# non-zero exit (124 timeout / crash) means the guest never powered off, which is exactly the
# timer/return failure class this increment fixed.
[ "$QEMU_RC" = 0 ]                                && echo "  [ok] qemu exited 0 (guest powered off cleanly; no post-DONE crash/hang)" || { echo "  [FAIL] qemu exit=$QEMU_RC (post-DONE crash / failed reset / timeout)"; ok=0; }

echo "======================================================"
if [ "$ok" = 1 ]; then
  echo "RESULT: PASS — public KlDatagram round-trip + DETACHED close over EFI_UDP4 on OVMF, no UDP leak"
  exit 0
else
  echo "RESULT: FAIL — the 7B-9 public-KlDatagram e2e did not meet acceptance"
  exit 1
fi
