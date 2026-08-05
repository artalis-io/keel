#!/usr/bin/env bash
# build_u5.sh — build the U-5 KlClient-with-DNS-over-EFI_UDP4 self-test into BOOTX64.EFI.
#
# Same freestanding self-contained archive as U-3, plus:
#   - U-1 shims:        allocator_uefi.c, platform_uefi.c
#   - U-2 socket:       socket_efi_tcp4.c (+ U-3 async-connect prims)
#   - U-3 completion:   event_efi.c
#   - U-5 DNS resolver: dns_uefi.c        (synchronous A-query over EFI_UDP4)
#   - resolver seam:    resolve_uefi.c    (numeric + static + -DKL_U5_DNS DNS path)
#   - link residuals:   u1_link_stubs.c   (-DKEEL_UEFI_HAVE_RESOLVE → resolve_uefi wins)
#   - entry:            u5_selftest.c
#
# The DNS path is gated by -DKL_U5_DNS (so U-3/U-4 default builds, which do not link
# dns_uefi.c, are unaffected). The nameserver + hostname are compile-time defines.
#
# Env:
#   CC                clang (default)
#   KEEL_ROOT         repo root holding the freestanding archive (default: ../..)
#   ARCHIVE           override the freestanding archive path
#   TARGET_PORT       responder port compiled into the URL (default 18080)
#   KL_U5_HOSTNAME    hostname the client resolves (default keel.test)
#   KL_U5_NAMESERVER  DNS server the guest queries (default 10.0.2.2)
set -euo pipefail
cd "$(dirname "$0")"

: "${CC:=clang}"
: "${KEEL_ROOT:=../..}"
: "${TARGET_PORT:=18080}"
: "${KL_U5_HOSTNAME:=keel.test}"
: "${KL_U5_NAMESERVER:=10.0.2.2}"
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
echo "hostname=$KL_U5_HOSTNAME nameserver=$KL_U5_NAMESERVER port=$TARGET_PORT"

CFLAGS=(
  --target=x86_64-unknown-windows
  -ffreestanding -fshort-wchar
  -fno-stack-protector -fno-builtin
  -mno-red-zone -std=c11
  -DKEEL_FREESTANDING
  -DTARGET_PORT="$TARGET_PORT"
  -DKL_U5_DNS
  -DKL_U5_NAMESERVER="\"$KL_U5_NAMESERVER\""
  -DKL_U5_HOSTNAME="\"$KL_U5_HOSTNAME\""
  -I"$KEEL_ROOT/include" -I"$KEEL_ROOT/vendor/llhttp" -I"$KEEL_ROOT/src"
  -isystem "$SHIM"
  -Wall -Wextra
  -c
)

# u1_link_stubs.c: -DKEEL_UEFI_HAVE_RESOLVE drops its fail-closed kl_resolve_sync in
# favor of resolve_uefi.c's (which now has the DNS path).
declare -A EXTRA=( [u1_link_stubs.c]="-DKEEL_UEFI_HAVE_RESOLVE" )

SRCS=( allocator_uefi.c platform_uefi.c socket_efi_tcp4.c event_efi.c
       dns_uefi.c resolve_uefi.c u1_link_stubs.c u5_selftest.c )
OBJS=()
for s in "${SRCS[@]}"; do
  o="${s%.c}.obj"
  echo "compiling $s ..."
  "$CC" "${CFLAGS[@]}" ${EXTRA[$s]:-} -o "$o" "$s"
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
