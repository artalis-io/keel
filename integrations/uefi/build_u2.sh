#!/usr/bin/env bash
# build_u2.sh — build the U-2 EFI_TCP4 socket-provider self-test into BOOTX64.EFI.
#
# Links libkeel_freestanding_selfcontained.a (mem*/strlen + kl_sockaddr_* in-archive,
# built by `make freestanding-lib-selfcontained` in the repo root) with the U-1 shim
# TUs (allocator_uefi.c, platform_uefi.c), the U-2 provider (socket_efi_tcp4.c), and
# the U-2 self-test entry (u2_selftest.c). Produces a PE32+ EFI application, NO hosted
# CRT (-nostdlib, lld PE).
#
# The U-2 self-test references the socket PROVIDER directly (not kl_client), so it does
# NOT pull the llhttp/client objects — the U-1 link stubs (__chkstk, abort, the
# fail-closed sockdef/event/DNS seams) are NOT needed and are deliberately omitted.
# Verified: the link closes with zero unresolved symbols against the archive alone.
#
# Env:
#   CC        clang (default)
#   KEEL_ROOT repo root holding the freestanding archive (default: ../..)
#   ARCHIVE   override the freestanding archive path
#   TARGET_PORT  responder port compiled into the GET (default 18080)
set -euo pipefail
cd "$(dirname "$0")"

: "${CC:=clang}"
: "${KEEL_ROOT:=../..}"
: "${TARGET_PORT:=18080}"
SHIM="$KEEL_ROOT/tests/freestanding/shim"
: "${ARCHIVE:=$KEEL_ROOT/libkeel_freestanding_selfcontained_x86_64.a}"

if [ ! -f "$ARCHIVE" ]; then
  ARCHIVE="$KEEL_ROOT/libkeel_freestanding_selfcontained.a"
fi
if [ ! -f "$ARCHIVE" ]; then
  echo "ERROR: freestanding archive not found. Run 'make freestanding-lib-selfcontained' in $KEEL_ROOT first." >&2
  exit 2
fi
echo "using archive: $ARCHIVE"

CFLAGS=(
  --target=x86_64-unknown-windows
  -ffreestanding -fshort-wchar
  -fno-stack-protector -fno-builtin
  -mno-red-zone -std=c11
  -DKEEL_FREESTANDING
  -DTARGET_PORT="$TARGET_PORT"
  -I"$KEEL_ROOT/include" -I"$KEEL_ROOT/vendor/llhttp" -I"$KEEL_ROOT/src"
  -isystem "$SHIM"
  -Wall -Wextra
  -c
)

SRCS=( allocator_uefi.c platform_uefi.c civil_time.c socket_efi_tcp4.c u2_selftest.c )
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
file BOOTX64.EFI || true
