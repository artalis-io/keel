#!/usr/bin/env bash
# run_u2.sh — U-7 harness. Runs inside the Ubuntu 24.04 container.
#   1. build the freestanding self-contained archive (make freestanding-lib-selfcontained)
#   2. build BOOTX64.EFI (build_u7.sh)
#   3. build a FAT ESP image with EFI/BOOT/BOOTX64.EFI + startup.nsh
#   4. start a python HTTP responder on the host (guest reaches it at 10.0.2.2:PORT)
#   5. boot QEMU x86_64 + OVMF (TCG, no KVM), e1000 NIC, SLIRP user-net, serial to stdout
#   6. capture serial, grep for U-7: markers / the status line
#
# Env:
#   KEEL_ROOT     repo root (default: ../..)
#   TARGET_PORT   host responder port (default 18080)
#   OVMF_CODE     override OVMF code fd
#   OVMF_VARS     override OVMF vars fd
#   BOOT_TIMEOUT  qemu timeout seconds (default 120)
set -uo pipefail
cd "$(dirname "$0")"

: "${KEEL_ROOT:=../..}"
TARGET_PORT="${TARGET_PORT:-18080}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-120}"
export TARGET_PORT

echo "=== U-7 run harness ==="

# ---- 1. build the freestanding self-contained archive (x86_64) ----
# Reuse a pre-built + already-gated x86_64 archive if present (some clang builds
# lower kl_cstr_builtin.c's byte loops back into mem* self-calls despite
# -fno-builtin, tripping the self-contained gate — a toolchain quirk, not a U-7
# issue). Set FORCE_ARCHIVE=1 to always rebuild.
# The self-contained gate + archive index need a COFF-aware nm/ar (the freestanding
# objects are PE/COFF). GNU binutils nm/ar on Linux cannot read clang's
# x86_64-unknown-windows objects (the gate then false-negatives "memcpy NOT defined"
# and lld-link cannot resolve the archive), so prefer llvm-nm/llvm-ar (any versioned
# variant). A macOS-built archive is NOT portable to Linux lld-link, so always
# rebuild here unless FORCE_ARCHIVE=0 is set with a Linux-built archive present.
pick() { for c in "$@"; do command -v "$c" >/dev/null 2>&1 && { echo "$c"; return; }; done; }
LLVM_NM="$(pick llvm-nm llvm-nm-18 llvm-nm-17 llvm-nm-16)"
LLVM_AR="$(pick llvm-ar llvm-ar-18 llvm-ar-17 llvm-ar-16)"
ARCHIVE_ARGS=()
[ -n "$LLVM_NM" ] && ARCHIVE_ARGS+=( "NM=$LLVM_NM" )
[ -n "$LLVM_AR" ] && ARCHIVE_ARGS+=( "AR=$LLVM_AR" )
echo "building libkeel_freestanding_selfcontained.a (${ARCHIVE_ARGS[*]:-default tools}) ..."
( cd "$KEEL_ROOT" && make freestanding-lib-selfcontained "${ARCHIVE_ARGS[@]}" ) \
  || { echo "ARCHIVE BUILD FAILED"; exit 2; }

# ---- 2. build the EFI app ----
TARGET_PORT="$TARGET_PORT" ./build_u7.sh || { echo "BUILD FAILED"; exit 2; }

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

# ---- 4. host HTTP responder ----
mkdir -p wwwroot
printf 'U-7 KlClient responder OK\n' > wwwroot/index.html
( cd wwwroot && python3 -m http.server "$TARGET_PORT" --bind 0.0.0.0 ) >/tmp/httpd_u7.log 2>&1 &
HTTPD_PID=$!
sleep 1
echo "python http.server pid=$HTTPD_PID on 0.0.0.0:$TARGET_PORT"
cleanup() { kill "$HTTPD_PID" 2>/dev/null || true; }
trap cleanup EXIT

# ---- 5. locate OVMF ----
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

# ---- 6. boot QEMU (TCG; no KVM in container) ----
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
timeout "${BOOT_TIMEOUT}" qemu-system-x86_64 "${QEMU_ARGS[@]}" 2>&1 | tee /tmp/serial_u7.log
echo "=== serial output end ==="

echo "=== grep markers ==="
grep -E 'U-7:|HTTP/1\.[01]' /tmp/serial_u7.log || echo "(no U-7 markers found)"
