#!/usr/bin/env bash
# build_dgram_public.sh — build the 7B-9 PUBLIC-KlDatagram-over-EFI_UDP4 self-test into BOOTX64.EFI.
#
# The runtime proof for 7B-9: the public keel/datagram.h facade (kl_datagram_init/recv_start/
# send/close_begin/free) over EFI_UDP4 on real firmware. Links the self-contained freestanding
# DATAGRAM archive (make freestanding-lib-dgram-selfcontained — udp + datagram_* + the public
# src/datagram.c + completion + event_ctx + in-archive mem*/strlen) — NO client/DNS archive, NO
# TLS. The transport is the unified EFI socket provider (SOCK_DGRAM → EFI_UDP4, socket_efi_udp4.c)
# + the datagram completion wiring in event_efi.c (post_dgram_recv/_send + drain, B.6 stable token).
#
#   - freestanding archive: libkeel_freestanding_dgram_selfcontained[_ARCH].a
#   - U-1 shims:        allocator_uefi.c, errno_uefi.c, platform_uefi.c, civil_time.c, wallclock_uefi.c
#   - unified socket:   socket_efi_tcp4.c (builder + provider) + socket_efi_udp4.c (EFI_UDP4 substrate)
#   - completion:       event_efi.c
#   - link residuals:   u1_link_stubs.c + dgram_link_stubs.c (fail-closed kl_sockdef_*/builtins)
#   - entry:            dgram_public_selftest.c
#
# Env:
#   CC                clang (default)
#   ARCH              x86_64 (default) | aarch64
#   KEEL_ROOT         repo root holding the freestanding archive (default: ../../../..)
#   ARCHIVE           override the freestanding datagram archive path
#   KL_U9_ECHO_PORT   host UDP echo port compiled into the probe dest (default 17777)
set -euo pipefail
cd "$(dirname "$0")"

: "${CC:=clang}"
: "${ARCH:=x86_64}"
: "${KEEL_ROOT:=../../../..}"
: "${KL_U9_ECHO_PORT:=17777}"
SHIM="$KEEL_ROOT/tests/freestanding/shim"

case "$ARCH" in
  x86_64)  TRIPLE=x86_64-unknown-windows;  BOOTNAME=BOOTX64.EFI; RZ=(-mno-red-zone) ;;
  aarch64) TRIPLE=aarch64-unknown-windows; BOOTNAME=BOOTAA64.EFI; RZ=() ;;
  *) echo "ERROR: unknown ARCH=$ARCH (want x86_64|aarch64)" >&2; exit 2 ;;
esac

: "${ARCHIVE:=$KEEL_ROOT/libkeel_freestanding_dgram_selfcontained_$ARCH.a}"
if [ ! -f "$ARCHIVE" ]; then ARCHIVE="$KEEL_ROOT/libkeel_freestanding_dgram_selfcontained.a"; fi
if [ ! -f "$ARCHIVE" ]; then
  echo "ERROR: freestanding datagram archive not found. Run 'make freestanding-lib-dgram-selfcontained' in $KEEL_ROOT first." >&2
  exit 2
fi
echo "arch=$ARCH triple=$TRIPLE"
echo "  datagram archive: $ARCHIVE"
echo "echo_port=$KL_U9_ECHO_PORT"

CFLAGS=(
  --target="$TRIPLE"
  -ffreestanding -fshort-wchar
  -fno-stack-protector -fno-builtin
  ${RZ[@]+"${RZ[@]}"} -std=c11
  -DKEEL_FREESTANDING
  -DKEEL_UEFI_DATAGRAM
  -DKL_U9_ECHO_PORT="$KL_U9_ECHO_PORT"
  -I"$KEEL_ROOT/include" -I"$KEEL_ROOT/vendor/llhttp" -I"$KEEL_ROOT/src"
  -I. -I..
  -isystem "$SHIM"
  -Wall -Wextra -Werror
  -c
)

SRCS=( ../allocator_uefi.c ../errno_uefi.c ../platform_uefi.c ../civil_time.c ../wallclock_uefi.c
       ../socket_efi_tcp4.c ../socket_efi_udp4.c ../event_efi.c
       u1_link_stubs.c dgram_link_stubs.c dgram_public_selftest.c )
OBJS=()
for s in "${SRCS[@]}"; do
  o="$(basename "${s%.c}").obj"
  echo "compiling $s ..."
  "$CC" "${CFLAGS[@]}" -o "$o" "$s"
  OBJS+=( "$o" )
done

echo "linking $BOOTNAME (lld PE, subsystem=efi_application, -nostdlib)..."
"$CC" --target="$TRIPLE" \
  -ffreestanding -fno-stack-protector -fno-builtin \
  -nostdlib -fuse-ld=lld \
  -Wl,-entry:efi_main -Wl,-subsystem:efi_application \
  -o "$BOOTNAME" "${OBJS[@]}" "$ARCHIVE"

echo "built: $(stat -c%s "$BOOTNAME" 2>/dev/null || stat -f%z "$BOOTNAME") bytes -> $BOOTNAME"
file "$BOOTNAME" || true
