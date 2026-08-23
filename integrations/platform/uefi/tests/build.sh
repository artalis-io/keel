#!/usr/bin/env bash
# build.sh — build the U-1 UEFI self-test into BOOTX64.EFI.
#
# Links libkeel_freestanding_selfcontained.a (mem*/strlen in-archive, built by
# `make freestanding-lib-selfcontained` in the repo root) with the two U-1 shim
# TUs (allocator_uefi.c, platform_uefi.c), the non-U-1 link stubs
# (u1_link_stubs.c), and the self-test entry (u1_selftest.c), producing a
# PE32+ EFI application with NO hosted CRT (-nostdlib, lld PE).
#
# clang/lld freestanding path (matches KEEL's freestanding toolchain):
#   clang --target=x86_64-unknown-windows -ffreestanding ... -c
#   clang ... -nostdlib -fuse-ld=lld -Wl,-subsystem:efi_application -> PE32+
#
# Env:
#   CC        clang (default)
#   KEEL_ROOT repo root holding the freestanding archive (default: ../../../..)
#   ARCHIVE   override the freestanding archive path
set -euo pipefail
cd "$(dirname "$0")"

: "${CC:=clang}"
: "${KEEL_ROOT:=../../../..}"
SHIM="$KEEL_ROOT/tests/freestanding/shim"
: "${ARCHIVE:=$KEEL_ROOT/libkeel_freestanding_selfcontained_x86_64.a}"

# Fall back to the canonical (x86_64-copied) archive name if the per-arch one
# is absent (native single-target build copies it to the base name).
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
  -I"$KEEL_ROOT/include" -I"$KEEL_ROOT/vendor/llhttp" -I"$KEEL_ROOT/src" -I. -I..
  -isystem "$SHIM"
  -Wall -Wextra
  -c
)

SRCS=( ../allocator_uefi.c ../platform_uefi.c ../wallclock_uefi.c ../civil_time.c u1_link_stubs.c u1_selftest.c )
OBJS=()
for s in "${SRCS[@]}"; do
  o="$(basename "${s%.c}").obj"
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
