#!/usr/bin/env bash
# run.sh: U-1 harness. Runs inside the Ubuntu 24.04 container.
#   1. build the freestanding self-contained archive (make freestanding-lib-selfcontained)
#   2. build BOOTX64.EFI (build.sh)
#   3. build a FAT ESP image with EFI/BOOT/BOOTX64.EFI + startup.nsh
#   4. boot QEMU x86_64 + OVMF (TCG, no KVM), serial to stdout
#   5. capture serial, grep for U-1: markers
#
# No network is needed for U-1 (allocator + clock + RNG only).
#
# Env:
#   KEEL_ROOT     repo root (default: ../../../..)
#   OVMF_CODE     override OVMF code fd
#   OVMF_VARS     override OVMF vars fd
#   BOOT_TIMEOUT  qemu timeout seconds (default 120)
set -uo pipefail
cd "$(dirname "$0")"

: "${KEEL_ROOT:=../../../..}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-120}"

echo "=== U-1 run harness ==="

# ---- 1. build the freestanding self-contained archive (x86_64) ----
echo "building libkeel_freestanding_selfcontained.a ..."
( cd "$KEEL_ROOT" && make freestanding-lib-selfcontained ) || { echo "ARCHIVE BUILD FAILED"; exit 2; }

# ---- 2. build the EFI app ----
./build.sh || { echo "BUILD FAILED"; exit 2; }

# ---- 3. ESP FAT image ----
ESP=esp.img
rm -f "$ESP"
dd if=/dev/zero of="$ESP" bs=1M count=48 status=none
mformat -i "$ESP" -F ::
mmd    -i "$ESP" ::/EFI
mmd    -i "$ESP" ::/EFI/BOOT
mcopy  -i "$ESP" BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
mcopy  -i "$ESP" startup.nsh ::/startup.nsh
echo "ESP image built:"
mdir -i "$ESP" ::/EFI/BOOT/

# ---- 4. locate OVMF ----
find_ovmf() {
  for c in \
    "${OVMF_CODE:-}" \
    /usr/share/OVMF/OVMF_CODE_4M.fd \
    /usr/share/OVMF/OVMF_CODE.fd \
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

OVMF_CODE_FD="$(find_ovmf)"
OVMF_VARS_FD="$(find_vars)"
if [ -z "$OVMF_CODE_FD" ]; then echo "ERROR: no OVMF code fd found"; exit 3; fi
echo "OVMF code: $OVMF_CODE_FD"
echo "OVMF vars: ${OVMF_VARS_FD:-<none>}"

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
  -serial stdio
  -display none
  -monitor none
  -no-reboot
)

echo "=== qemu cmd ==="
echo "timeout ${BOOT_TIMEOUT} qemu-system-x86_64 ${QEMU_ARGS[*]}"
echo "=== serial output begin ==="
timeout "${BOOT_TIMEOUT}" qemu-system-x86_64 "${QEMU_ARGS[@]}" 2>&1 | tee /tmp/serial_u1.log
echo "=== serial output end ==="

echo "=== grep markers ==="
grep -E 'U-1:' /tmp/serial_u1.log || echo "(no U-1 markers found)"
