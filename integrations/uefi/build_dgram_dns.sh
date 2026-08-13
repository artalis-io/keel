#!/usr/bin/env bash
# build_dgram_dns.sh — build the 6.4c stock-dns_resolver-over-EFI_UDP4 self-test into BOOTX64.EFI.
#
# Unlike build_u5.sh (which linked the bespoke one-shot dns_uefi.c behind kl_resolve_sync),
# this links the STOCK src/dns_resolver.c — the real async Do53 engine — over KlUdp-over-
# EFI_UDP4: the unified EFI socket provider (SOCK_DGRAM → EFI_UDP4, socket_efi_udp4.c) +
# the datagram completion wiring in event_efi.c. The stock resolver + datagram machine come
# from the self-contained freestanding DNS archive; only the EFI-native TUs are compiled here.
#
#   - freestanding archive: libkeel_freestanding_dns_selfcontained[_ARCH].a
#                           (make freestanding-lib-dns-selfcontained) — dns_resolver + udp +
#                           datagram_* + completion + event_ctx + kl_cstr_builtin (mem*/strlen)
#   - U-1 shims:        allocator_uefi.c, errno_uefi.c, platform_uefi.c, civil_time.c, wallclock_uefi.c
#   - unified socket:   socket_efi_tcp4.c (builder + provider) + socket_efi_udp4.c (EFI_UDP4 substrate)
#   - completion:       event_efi.c (post_dgram_recv/_send + drain, B.6 stable token)
#   - entry:            dgram_dns_selftest.c
#
# Env:
#   CC                clang (default)
#   ARCH              x86_64 (default) | aarch64
#   KEEL_ROOT         repo root holding the freestanding archive (default: ../..)
#   ARCHIVE           override the freestanding archive path
#   KL_U9_HOSTNAME    hostname the resolver looks up (default keel.test)
#   KL_U9_NAMESERVER  DNS server the guest queries (default 10.0.2.2 — the SLIRP host)
set -euo pipefail
cd "$(dirname "$0")"

: "${CC:=clang}"
: "${ARCH:=x86_64}"
: "${KEEL_ROOT:=../..}"
: "${KL_U9_HOSTNAME:=keel.test}"
: "${KL_U9_NAMESERVER:=10.0.2.2}"
SHIM="$KEEL_ROOT/tests/freestanding/shim"

case "$ARCH" in
  x86_64)  TRIPLE=x86_64-unknown-windows;  BOOTNAME=BOOTX64.EFI; RZ=(-mno-red-zone) ;;
  aarch64) TRIPLE=aarch64-unknown-windows; BOOTNAME=BOOTAA64.EFI; RZ=() ;;
  *) echo "ERROR: unknown ARCH=$ARCH (want x86_64|aarch64)" >&2; exit 2 ;;
esac

: "${ARCHIVE:=$KEEL_ROOT/libkeel_freestanding_dns_selfcontained_$ARCH.a}"
if [ ! -f "$ARCHIVE" ]; then ARCHIVE="$KEEL_ROOT/libkeel_freestanding_dns_selfcontained.a"; fi
if [ ! -f "$ARCHIVE" ]; then
  echo "ERROR: freestanding DNS archive not found. Run 'make freestanding-lib-dns-selfcontained' in $KEEL_ROOT first." >&2
  exit 2
fi
echo "arch=$ARCH triple=$TRIPLE archive=$ARCHIVE"
echo "hostname=$KL_U9_HOSTNAME nameserver=$KL_U9_NAMESERVER"

CFLAGS=(
  --target="$TRIPLE"
  -ffreestanding -fshort-wchar
  -fno-stack-protector -fno-builtin
  ${RZ[@]+"${RZ[@]}"} -std=c11
  -DKEEL_FREESTANDING
  -DKL_U9_NAMESERVER="\"$KL_U9_NAMESERVER\""
  -DKL_U9_HOSTNAME="\"$KL_U9_HOSTNAME\""
  -I"$KEEL_ROOT/include" -I"$KEEL_ROOT/vendor/llhttp" -I"$KEEL_ROOT/src"
  -I. -I"$KEEL_ROOT/spikes/uefi"
  -isystem "$SHIM"
  -Wall -Wextra
  -c
)

# u1_link_stubs.c supplies the fail-closed DEFAULT provider residuals (kl_sockdef_*,
# kl_event_*_builtin, kl_comp_ops_builtin) the archive references but that the injected
# EFI providers replace at runtime; its unused kl_resolve_sync is dead weight (the async
# KlResolver path never calls it — contrast U-5).
SRCS=( allocator_uefi.c errno_uefi.c platform_uefi.c civil_time.c wallclock_uefi.c
       socket_efi_tcp4.c socket_efi_udp4.c event_efi.c
       u1_link_stubs.c dgram_link_stubs.c dgram_dns_selftest.c )
OBJS=()
for s in "${SRCS[@]}"; do
  o="${s%.c}.obj"
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
