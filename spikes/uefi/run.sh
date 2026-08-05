#!/usr/bin/env bash
# run.sh — U-0 harness. Runs inside the Ubuntu 24.04 container.
#   1. build BOOTX64.EFI (build.sh)
#   2. build a FAT ESP image with EFI/BOOT/BOOTX64.EFI + startup.nsh
#   3. start a python HTTP responder on the host (reached by guest at 10.0.2.2:PORT)
#   4. boot QEMU x86_64 + OVMF, e1000 NIC, SLIRP user-net, serial to stdout
#   5. capture serial, grep for the status line / U-0: GO
#
# Env:
#   TARGET_PORT   host responder port (default 18080)
#   OVMF_CODE     override OVMF code fd
#   OVMF_VARS     override OVMF vars fd
#   NETWORK_OVMF  path to a network-enabled OVMF_CODE.fd to try (optional)
#   BOOT_TIMEOUT  qemu timeout seconds (default 120)
set -uo pipefail
cd "$(dirname "$0")"

TARGET_PORT="${TARGET_PORT:-18080}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-120}"
export TARGET_PORT

echo "=== U-0 run harness ==="

# ---- 1. build the EFI app ----
./build.sh || { echo "BUILD FAILED"; exit 2; }

# ---- 2. ESP FAT image ----
ESP=esp.img
rm -f "$ESP"
# 48 MiB FAT image
dd if=/dev/zero of="$ESP" bs=1M count=48 status=none
mformat -i "$ESP" -F ::
mmd    -i "$ESP" ::/EFI
mmd    -i "$ESP" ::/EFI/BOOT
mcopy  -i "$ESP" BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
mcopy  -i "$ESP" startup.nsh ::/startup.nsh
echo "ESP image built:"
mdir -i "$ESP" ::/EFI/BOOT/

# ---- 3. host HTTP responder ----
mkdir -p wwwroot
printf 'U-0 spike responder OK\n' > wwwroot/index.html
( cd wwwroot && python3 -m http.server "$TARGET_PORT" --bind 0.0.0.0 ) >/tmp/httpd.log 2>&1 &
HTTPD_PID=$!
sleep 1
echo "python http.server pid=$HTTPD_PID on 0.0.0.0:$TARGET_PORT"

cleanup() { kill "$HTTPD_PID" 2>/dev/null || true; }
trap cleanup EXIT

# ---- 4. locate OVMF ----
find_ovmf() {
  for c in \
    "${OVMF_CODE:-}" \
    /usr/share/OVMF/OVMF_CODE_4M.fd \
    /usr/share/OVMF/OVMF_CODE.fd \
    /usr/share/OVMF/OVMF_CODE.secboot.fd \
    /usr/share/ovmf/OVMF.fd ; do
    [ -n "$c" ] && [ -f "$c" ] && { echo "$c"; return; }
  done
}
find_vars() {
  for v in \
    "${OVMF_VARS:-}" \
    /usr/share/OVMF/OVMF_VARS_4M.fd \
    /usr/share/OVMF/OVMF_VARS.fd ; do
    [ -n "$v" ] && [ -f "$v" ] && { echo "$v"; return; }
  done
}

OVMF_CODE_FD="${NETWORK_OVMF:-$(find_ovmf)}"
OVMF_VARS_FD="$(find_vars)"
if [ -z "$OVMF_CODE_FD" ]; then echo "ERROR: no OVMF code fd found"; exit 3; fi
echo "OVMF code: $OVMF_CODE_FD"
echo "OVMF vars: ${OVMF_VARS_FD:-<none>}"
echo "--- ovmf package contents ---"
dpkg -L ovmf 2>/dev/null | grep -Ei '\.fd$' || true

# writable copy of vars
VARS_RW=vars_rw.fd
if [ -n "$OVMF_VARS_FD" ]; then cp "$OVMF_VARS_FD" "$VARS_RW"; fi

# ---- 5. boot QEMU (TCG; no KVM in container) ----
QEMU_ARGS=(
  -machine q35
  -m 512
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE_FD"
)
[ -n "$OVMF_VARS_FD" ] && QEMU_ARGS+=( -drive if=pflash,format=raw,file="$VARS_RW" )
QEMU_ARGS+=(
  -drive format=raw,file="$ESP"
  -netdev "user,id=n0"
  -device e1000,netdev=n0
  -serial stdio
  -display none
  -monitor none
  -no-reboot
)

echo "=== qemu cmd ==="
echo "timeout ${BOOT_TIMEOUT} qemu-system-x86_64 ${QEMU_ARGS[*]}"
echo "=== serial output begin ==="
timeout "${BOOT_TIMEOUT}" qemu-system-x86_64 "${QEMU_ARGS[@]}" 2>&1 | tee /tmp/serial.log
echo "=== serial output end ==="

echo "=== grep markers ==="
grep -E 'U-0:|HTTP/1\.1' /tmp/serial.log || echo "(no U-0 markers found)"
