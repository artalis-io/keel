#!/usr/bin/env bash
# build.sh — build tcp4_get.c into BOOTX64.EFI (PE32+ EFI application).
#
# clang/lld freestanding path (matches KEEL's freestanding toolchain, no gnu-efi runtime):
#   clang --target=x86_64-unknown-windows -ffreestanding ... -c
#   lld-link /subsystem:efi_application /entry:efi_main -> PE32+
#
# Runs inside the Ubuntu 24.04 container (see run.sh). Produces BOOTX64.EFI here.
set -euo pipefail
cd "$(dirname "$0")"

: "${TARGET_PORT:=18080}"
: "${CC:=clang}"

CFLAGS=(
  --target=x86_64-unknown-windows
  -ffreestanding -fno-stack-protector -fshort-wchar
  -mno-red-zone -std=c11
  -Wall -Wextra
  -DTARGET_PORT="${TARGET_PORT}"
  -c
)

echo "compiling tcp4_get.c (clang freestanding, x86_64 PE)..."
"$CC" "${CFLAGS[@]}" -o tcp4_get.obj tcp4_get.c

echo "linking BOOTX64.EFI (lld-link, subsystem=efi_application)..."
lld-link \
  /subsystem:efi_application \
  /entry:efi_main \
  /nodefaultlib \
  /out:BOOTX64.EFI \
  tcp4_get.obj

echo "built: $(stat -c%s BOOTX64.EFI 2>/dev/null || stat -f%z BOOTX64.EFI) bytes -> BOOTX64.EFI"
file BOOTX64.EFI || true
