#!/usr/bin/env bash
# build_host_map_test.sh — build + run the U-2 host mapping unit test (no QEMU).
#
# Compiles socket_efi_tcp4.c + allocator_uefi.c + host_map_test.c for the HOST and
# links libkeel.a (for kl_sockaddr_*). Only the PURE mapping functions are exercised;
# the EFI ops table is present but never called (no firmware). This proves the
# EFI_STATUS→KlIoStatus/KlError + KlSockAddr⇄EFI_IPv4_ADDRESS mappings WITHOUT QEMU.
#
# Env: CC (default cc), KEEL_ROOT (default ../../../..)
set -euo pipefail
cd "$(dirname "$0")"

: "${CC:=cc}"
: "${KEEL_ROOT:=../../../..}"

LIB="$KEEL_ROOT/libkeel.a"
if [ ! -f "$LIB" ]; then
  echo "building libkeel.a ..."
  ( cd "$KEEL_ROOT" && make -j4 >/dev/null )
fi

CFLAGS=( -std=c11 -Wall -Wextra -Werror
         -I"$KEEL_ROOT/include" -I"$KEEL_ROOT/src" -I. -I.. )

echo "compiling host map test ..."
"$CC" "${CFLAGS[@]}" -c ../socket_efi_tcp4.c -o /tmp/u2_socket_efi.o
"$CC" "${CFLAGS[@]}" -c ../allocator_uefi.c  -o /tmp/u2_alloc_uefi.o
"$CC" "${CFLAGS[@]}" -c host_map_test.c   -o /tmp/u2_host_map.o

echo "linking ..."
# GNU ld makes a single pass over an archive, so a backward intra-libkeel.a
# reference pulled by socket_efi_tcp4.o (kl_sockaddr_*/kl_malloc) can be left
# unresolved. macOS ld64 rescans and is fine; wrap in a group for GNU ld.
# ld64 accepts --start-group/--end-group as a no-op, so this stays portable.
GROUP=""
case "$(uname -s)" in Linux) GROUP="-Wl,--start-group" ; GROUPEND="-Wl,--end-group" ;; *) GROUP="" ; GROUPEND="" ;; esac
"$CC" -o /tmp/u2_host_map_test \
  /tmp/u2_host_map.o /tmp/u2_socket_efi.o /tmp/u2_alloc_uefi.o $GROUP "$LIB" $GROUPEND

echo "=== running ==="
/tmp/u2_host_map_test
