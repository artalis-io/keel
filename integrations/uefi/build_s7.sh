#!/usr/bin/env bash
# build_s7.sh — build the S-7 KlServer-over-EFI_TCP4 self-test into BOOTX64.EFI.
#
# Links libkeel_freestanding_server_selfcontained.a (the model-blind server core +
# protocol layer + mem*/strlen in-archive, `make freestanding-lib-server-selfcontained`)
# with:
#   - U-1 shims:        allocator_uefi.c, platform_uefi.c (+ errno/time support)
#   - EFI socket:       socket_efi_tcp4.c (bind/listen/accept — S-2)
#   - EFI completion:   event_efi.c (accept relay + prime_accepts — S-3)
#   - link residuals:   u1_link_stubs.c (abort/fprintf/__chkstk/_fltused + fail-closed
#                       sockdef/event builtins the injected providers override)
#   - entry:            s4_selftest.c
#
# The server pulls NO client / resolver objects (no resolve_uefi.c) — it serves, it does
# not connect. Produces a PE32+ EFI application, NO hosted CRT (-nostdlib, lld PE).
#
# Env:
#   CC           clang (default)
#   KEEL_ROOT    repo root holding the freestanding archive (default: ../..)
#   ARCHIVE      override the freestanding server archive path
set -euo pipefail
cd "$(dirname "$0")"

: "${CC:=clang}"
: "${KEEL_ROOT:=../..}"
SHIM="$KEEL_ROOT/tests/freestanding/shim"
: "${ARCHIVE:=$KEEL_ROOT/libkeel_freestanding_server_selfcontained_x86_64.a}"

if [ ! -f "$ARCHIVE" ]; then
  ARCHIVE="$KEEL_ROOT/libkeel_freestanding_server_selfcontained.a"
fi
if [ ! -f "$ARCHIVE" ]; then
  echo "ERROR: freestanding server archive not found. Run 'make freestanding-lib-server-selfcontained' in $KEEL_ROOT first." >&2
  exit 2
fi
echo "using archive: $ARCHIVE"

CFLAGS=(
  --target=x86_64-unknown-windows
  -ffreestanding -fshort-wchar
  -fno-stack-protector -fno-builtin
  -mno-red-zone -std=c11
  -DKEEL_FREESTANDING
  -I"$KEEL_ROOT/include" -I"$KEEL_ROOT/vendor/llhttp" -I"$KEEL_ROOT/src"
  -isystem "$SHIM"
  -Wall -Wextra
  -c
)

SRCS=( allocator_uefi.c errno_uefi.c platform_uefi.c civil_time.c wallclock_uefi.c
       socket_efi_tcp4.c event_efi.c lifecycle_uefi.c u1_link_stubs.c s4_link_stubs.c s7_selftest.c )
OBJS=()
for s in "${SRCS[@]}"; do
  o="${s%.c}.obj"
  echo "compiling $s ..."
  "$CC" "${CFLAGS[@]}" -o "$o" "$s"
  OBJS+=( "$o" )
done

echo "linking BOOTX64.EFI (lld PE, subsystem=efi_application, -nostdlib)..."
"$CC" --target=x86_64-unknown-windows \
  -ffreestanding -fno-stack-protector -fno-builtin \
  -nostdlib -fuse-ld=lld \
  -Wl,-entry:efi_main -Wl,-subsystem:efi_application \
  -o BOOTX64.EFI "${OBJS[@]}" "$ARCHIVE"

echo "built: $(stat -c%s BOOTX64.EFI 2>/dev/null || stat -f%z BOOTX64.EFI) bytes -> BOOTX64.EFI"
command -v file >/dev/null 2>&1 && file BOOTX64.EFI || true
