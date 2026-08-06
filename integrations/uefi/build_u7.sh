#!/usr/bin/env bash
# build_u7.sh — build the U-7 KlClient-over-EFI_TCP4 self-test into BOOTX64.EFI.
#
# Links libkeel_freestanding_selfcontained.a (mem*/strlen + kl_sockaddr_*/client/
# llhttp in-archive, `make freestanding-lib-selfcontained`) with:
#   - U-1 shims:        allocator_uefi.c, platform_uefi.c
#   - U-2 socket:       socket_efi_tcp4.c (+ U-7 async-connect prims)
#   - U-7 completion:   event_efi.c
#   - numeric resolver: resolve_uefi.c   (pre-DNS kl_resolve_sync)
#   - link residuals:   u1_link_stubs.c  (abort/fprintf/__chkstk + fail-closed
#                       sockdef/event builtins; compiled -DKEEL_UEFI_HAVE_RESOLVE so
#                       its fail-closed kl_resolve_sync yields to resolve_uefi.c)
#   - entry:            u7_selftest.c
#
# Unlike U-2, this self-test drives kl_client, so it pulls the client/llhttp objects
# and thus needs the u1_link_stubs residuals (__chkstk for the llhttp state machine's
# >4 KiB frame; abort/fprintf for vendored-llhttp's unreachable defaults). Produces a
# PE32+ EFI application, NO hosted CRT (-nostdlib, lld PE).
#
# Env:
#   CC           clang (default)
#   KEEL_ROOT    repo root holding the freestanding archive (default: ../..)
#   ARCHIVE      override the freestanding archive path
#   TARGET_PORT  responder port compiled into the URL (default 18080)
#   TARGET_HOST  responder host compiled into the URL (default 10.0.2.2)
set -euo pipefail
if [ "${BASH_VERSINFO:-0}" -lt 4 ]; then echo "ERROR: this script needs bash 4+ (uses associative arrays); on macOS run: brew install bash, or run it in the container." >&2; exit 3; fi
cd "$(dirname "$0")"

: "${CC:=clang}"
: "${KEEL_ROOT:=../..}"
: "${TARGET_PORT:=18080}"
: "${TARGET_HOST:=10.0.2.2}"
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
  -DTARGET_HOST="\"$TARGET_HOST\""
  -I"$KEEL_ROOT/include" -I"$KEEL_ROOT/vendor/llhttp" -I"$KEEL_ROOT/src"
  -isystem "$SHIM"
  -Wall -Wextra
  -c
)

# u1_link_stubs.c gets an extra define so its fail-closed kl_resolve_sync is dropped
# in favor of resolve_uefi.c's numeric one.
declare -A EXTRA=( [u1_link_stubs.c]="-DKEEL_UEFI_HAVE_RESOLVE" )

SRCS=( allocator_uefi.c platform_uefi.c civil_time.c wallclock_uefi.c socket_efi_tcp4.c event_efi.c
       resolve_uefi.c lifecycle_uefi.c u1_link_stubs.c u7_selftest.c )
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
