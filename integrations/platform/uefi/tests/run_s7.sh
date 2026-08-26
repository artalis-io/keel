#!/usr/bin/env bash
# run_s4.sh: S-7 harness. Runs inside the Ubuntu 24.04 container.
#   1. build the freestanding self-contained SERVER archive
#      (make freestanding-lib-server-selfcontained)
#   2. build BOOTX64.EFI (build_s7.sh): the EFI_TCP4 plaintext HTTP server
#   3. build a FAT ESP image with EFI/BOOT/BOOTX64.EFI + startup.nsh
#   4. boot QEMU x86_64 + OVMF (TCG, no KVM), e1000 NIC, SLIRP user-net with a
#      host->guest port-forward (hostfwd tcp::HOST_PORT-:80), serial to a log (bg)
#   5. wait for "S-7: listening on :80" on the serial, then curl the forwarded port
#      from the host; expect an HTTP 200 + the "S-7: GO" marker on serial
#   6. tear QEMU down
#
# Unlike the client harnesses (U-3/U-4), the workload runs IN the guest and the host
# is the client, so this needs a SLIRP hostfwd (guest :80 -> host :HOST_PORT), not a
# guest->host responder.
#
# Env:
#   KEEL_ROOT     repo root (default: ../../../..)
#   HOST_PORT     host side of the port-forward (default 18080)
#   OVMF_CODE / OVMF_VARS   override OVMF fds
#   BOOT_TIMEOUT  qemu timeout seconds (default 240; DHCP + TCG boot is slow)
set -uo pipefail
cd "$(dirname "$0")"

: "${KEEL_ROOT:=../../../..}"
HOST_PORT="${HOST_PORT:-18080}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-240}"

echo "=== S-7 run harness (plaintext HTTP server over EFI_TCP4) ==="

# ---- 1. freestanding self-contained SERVER archive (x86_64), COFF-aware nm/ar ----
pick() { for c in "$@"; do command -v "$c" >/dev/null 2>&1 && { echo "$c"; return; }; done; }
LLVM_NM="$(pick llvm-nm llvm-nm-18 llvm-nm-17 llvm-nm-16)"
LLVM_AR="$(pick llvm-ar llvm-ar-18 llvm-ar-17 llvm-ar-16)"
ARCHIVE_ARGS=()
[ -n "$LLVM_NM" ] && ARCHIVE_ARGS+=( "NM=$LLVM_NM" )
[ -n "$LLVM_AR" ] && ARCHIVE_ARGS+=( "AR=$LLVM_AR" )
echo "building libkeel_freestanding_server_selfcontained.a (${ARCHIVE_ARGS[*]:-default tools}) ..."
( cd "$KEEL_ROOT" && make freestanding-lib-server-selfcontained "${ARCHIVE_ARGS[@]}" ) \
  || { echo "ARCHIVE BUILD FAILED"; exit 2; }

# ---- 2. build the EFI app ----
./build_s7.sh || { echo "BUILD FAILED"; exit 2; }

# ---- 3. ESP FAT image ----
ESP=esp_s7.img
rm -f "$ESP"
dd if=/dev/zero of="$ESP" bs=1M count=64 status=none
mformat -i "$ESP" -F ::
mmd    -i "$ESP" ::/EFI
mmd    -i "$ESP" ::/EFI/BOOT
mcopy  -i "$ESP" BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
mcopy  -i "$ESP" startup.nsh ::/startup.nsh
echo "ESP image built:"
mdir -i "$ESP" ::/EFI/BOOT/

# ---- 4. locate OVMF ----
find_ovmf() {
  for c in "${OVMF_CODE:-}" \
    /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/ovmf/OVMF.fd ; do
    [ -n "$c" ] && [ -f "$c" ] && { echo "$c"; return; }
  done
}
find_vars() {
  for v in "${OVMF_VARS:-}" \
    /usr/share/OVMF/OVMF_VARS_4M.fd /usr/share/OVMF/OVMF_VARS.fd ; do
    [ -n "$v" ] && [ -f "$v" ] && { echo "$v"; return; }
  done
}
OVMF_CODE_FD="$(find_ovmf)"
OVMF_VARS_FD="$(find_vars)"
if [ -z "$OVMF_CODE_FD" ]; then echo "ERROR: no OVMF code fd found"; exit 3; fi
echo "OVMF code: $OVMF_CODE_FD"
echo "OVMF vars: ${OVMF_VARS_FD:-<none>}"
VARS_RW=vars_rw_s7.fd
if [ -n "$OVMF_VARS_FD" ]; then cp "$OVMF_VARS_FD" "$VARS_RW"; fi

# ---- 5. boot QEMU (TCG; no KVM in container), serial -> log, in the BACKGROUND so
# the host can curl the forwarded port while the guest serves ----
SERIAL=/tmp/serial_s7.log
rm -f "$SERIAL"
QEMU_ARGS=(
  -machine q35
  -m 512
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE_FD"
)
[ -n "$OVMF_VARS_FD" ] && QEMU_ARGS+=( -drive if=pflash,format=raw,file="$VARS_RW" )
QEMU_ARGS+=(
  -drive format=raw,file="$ESP"
  -netdev "user,id=n0,hostfwd=tcp::${HOST_PORT}-:80"
  -device e1000,netdev=n0
  -serial file:"$SERIAL"
  -display none
  -monitor none
  -no-reboot
)
echo "=== qemu cmd ==="
echo "qemu-system-x86_64 ${QEMU_ARGS[*]}"
timeout "${BOOT_TIMEOUT}" qemu-system-x86_64 "${QEMU_ARGS[@]}" &
QEMU_PID=$!
cleanup() { kill "$QEMU_PID" 2>/dev/null || true; }
trap cleanup EXIT

# ---- 6. wait for the listener, then curl the forwarded port ----
echo "waiting for 'S-7: listening on :80' on the guest serial (up to ${BOOT_TIMEOUT}s) ..."
listening=0
for _ in $(seq 1 "${BOOT_TIMEOUT}"); do
  if ! kill -0 "$QEMU_PID" 2>/dev/null; then echo "qemu exited early"; break; fi
  if grep -q "S-7: listening on :80" "$SERIAL" 2>/dev/null; then listening=1; break; fi
  sleep 1
done

status=""
if [ "$listening" = 1 ]; then
  echo "guest is listening; curling http://127.0.0.1:${HOST_PORT}/ ..."
  # a few attempts; the EFI_TCP4 accept path may need a moment to prime
  for _ in $(seq 1 20); do
    status="$(curl -s -o /tmp/s7_body.txt -w '%{http_code}' --max-time 5 \
                "http://127.0.0.1:${HOST_PORT}/" 2>/dev/null || true)"
    [ "$status" = "200" ] && break
    sleep 1
  done
  echo "curl HTTP status = ${status:-<none>}"
  echo "--- response body ---"; cat /tmp/s7_body.txt 2>/dev/null; echo "--- end ---"
else
  echo "guest never reported 'listening'; see serial log below"
fi

# give the serial a moment to flush the GO marker, then report
sleep 2
echo "=== serial markers ==="
grep -E 'S-7:' "$SERIAL" 2>/dev/null || echo "(no S-7 markers found)"

# After curl gets its 200 the guest serves the grace tail, tears down, and prints the
# clean-teardown markers. Give it a moment, then require BOTH the 200 and "S-7: PASS"
# (served + kl_http_server_free + 0 live sockets + kl_uefi_shutdown).
for _ in $(seq 1 30); do grep -q "S-7: PASS\|S-7: FAIL" "$SERIAL" 2>/dev/null && break; sleep 1; done
if [ "$status" = "200" ] && grep -q "S-7: PASS" "$SERIAL" 2>/dev/null; then
  echo "S-7: PASS, served GET / -> 200 then clean teardown (0 live sockets, providers released)"
  exit 0
fi
echo "S-7: FAIL (status=${status:-<none>}, teardown marker $(grep -q 'S-7: PASS' "$SERIAL" 2>/dev/null && echo PASS || echo absent))"
echo "=== serial tail ==="; tail -30 "$SERIAL" 2>/dev/null || true
exit 1
