#!/usr/bin/env bash
# build_mock_efi_test.sh — HOST build of the mock-EFI failure-path harness (F7b).
#
# Compiles the REAL provider TUs (socket_efi_tcp4.c, dns_uefi.c, event_efi.c,
# allocator_uefi.c) host-side against the scriptable fake EFI in mock_efi_test.c, under
# ASan+UBSan, and links the core libkeel.a for kl_sockaddr_*/kl_malloc/etc. Runs the
# token-lifetime failure-path assertions (timeouts, aborts, close-with-outstanding,
# post-EBS, stale-guard). This is the load-bearing verification the QEMU happy-path
# cannot provide.
#
# EFIAPI is gated on __x86_64__ (spikes/uefi/efi_min.h): on an arm64 host it is empty
# (AAPCS), on x86_64 it is ms_abi — either way the mock vtables and the provider's calls
# agree, so the fake firmware is ABI-compatible with the provider on the host.
set -euo pipefail
cd "$(dirname "$0")"
: "${CC:=cc}"
: "${KEEL_ROOT:=../..}"

[ -f "$KEEL_ROOT/libkeel.a" ] || ( cd "$KEEL_ROOT" && make -j4 >/dev/null )

CFLAGS=(
  -std=c11 -g -O1 -fno-omit-frame-pointer
  -fsanitize=address,undefined -fno-sanitize-recover=all
  -I"$KEEL_ROOT/include" -I"$KEEL_ROOT/src" -I. -I"$KEEL_ROOT/spikes/uefi"
  -Wall -Wextra
)
SRCS=( mock_efi_test.c socket_efi_tcp4.c dns_uefi.c event_efi.c allocator_uefi.c )

echo "building mock_efi_test (host, ASan+UBSan) ..."
"$CC" "${CFLAGS[@]}" "${SRCS[@]}" "$KEEL_ROOT/libkeel.a" -o mock_efi_test
echo "running ..."
ASAN_OPTIONS=detect_leaks=1 ./mock_efi_test
