#!/usr/bin/env bash
# build_s6.sh — build the S-6 KlServer HTTPS-over-EFI_TCP4+mbedTLS self-test into
# BOOTX64.EFI. The server mirror of build_u4.sh (which builds the HTTPS *client*):
#   - compile ALL mbedTLS library/*.c freestanding with the U-4 config.
#   - compile the Keel mbedTLS adapter + the mbedTLS platform TUs (entropy/time/heap).
#   - compile the U-1/U-2/U-3 Keel UEFI TUs (allocator/platform/socket/event) +
#     u1_link_stubs.c (client residuals) + s4_link_stubs.c (server residuals) +
#     s6_selftest.c (entry). NO resolve_uefi.c — a server serves, it does not connect.
#   - link with the freestanding SERVER self-contained archive -> BOOTX64.EFI.
#
# Requires MBEDTLS_SRC to point at an extracted mbedTLS release tarball tree; run_s6.sh
# downloads it and generates s6_cert_embed.h (the embedded server cert + key).
#
# Env:
#   CC           clang (default)
#   KEEL_ROOT    repo root holding the freestanding server archive (default: ../..)
#   ARCHIVE      override the freestanding server archive path
#   MBEDTLS_SRC  extracted mbedTLS release tarball tree (REQUIRED)
set -euo pipefail
cd "$(dirname "$0")"

: "${CC:=clang}"
: "${KEEL_ROOT:=../..}"
: "${MBEDTLS_SRC:?set MBEDTLS_SRC to an extracted mbedTLS release tarball tree (include/ + library/)}"

if [ ! -d "$MBEDTLS_SRC/library" ] || [ ! -d "$MBEDTLS_SRC/include" ]; then
  echo "ERROR: MBEDTLS_SRC=$MBEDTLS_SRC is missing library/ or include/" >&2; exit 2
fi
[ -f s6_cert_embed.h ] || { echo "ERROR: s6_cert_embed.h missing (run_s6.sh generates it)"; exit 2; }

SHIM="$KEEL_ROOT/tests/freestanding/shim"
LOCAL_SHIM="./mbedtls_shim"
MBEDTLS_ADAPTER="$KEEL_ROOT/integrations/mbedtls"
: "${ARCHIVE:=$KEEL_ROOT/libkeel_freestanding_server_selfcontained_x86_64.a}"
if [ ! -f "$ARCHIVE" ]; then ARCHIVE="$KEEL_ROOT/libkeel_freestanding_server_selfcontained.a"; fi
if [ ! -f "$ARCHIVE" ]; then
  echo "ERROR: freestanding server archive not found. Run 'make freestanding-lib-server-selfcontained' first." >&2
  exit 2
fi
echo "using archive:     $ARCHIVE"
echo "using mbedtls src: $MBEDTLS_SRC"

BASE_TARGET=(
  --target=x86_64-unknown-windows
  -ffreestanding -fshort-wchar
  -fno-stack-protector -fno-builtin -fno-autolink
  -mno-red-zone -std=c11
)
MBEDTLS_DEFS=(
  -DMBEDTLS_CONFIG_FILE='"mbedtls_config_uefi.h"'
  -DMBEDTLS_TEST_SW_INET_PTON
)
MBEDTLS_INCS=(
  -I"$MBEDTLS_SRC/include"
  -I.
  -isystem "$LOCAL_SHIM"
  -isystem "$SHIM"
)
OBJS=()

# ---- 1. mbedTLS library/*.c (freestanding, warnings relaxed like vendor code) ----
echo "compiling mbedTLS library/*.c (freestanding, -w) ..."
mkdir -p mbedtls_obj
for src in "$MBEDTLS_SRC"/library/*.c; do
  o="mbedtls_obj/$(basename "$src" .c).obj"
  "$CC" "${BASE_TARGET[@]}" "${MBEDTLS_DEFS[@]}" "${MBEDTLS_INCS[@]}" \
    -ffunction-sections -fdata-sections -w -c -o "$o" "$src"
  OBJS+=( "$o" )
done
echo "  compiled $(ls mbedtls_obj/*.obj | wc -l) mbedTLS objects"

# Entropy: the S-6 spike opts into the INSECURE weak-entropy fallback so the demo runs
# without a virtio-rng device (matches the loud runtime warning). A production build drops
# this define and requires real EFI_RNG (fail-closed) — see build_u4.sh U4_MODE=prod.
ENTROPY_DEFS=( -DKL_UEFI_INSECURE_TEST_ENTROPY )
echo "entropy policy:    INSECURE test fallback (spike; -DKL_UEFI_INSECURE_TEST_ENTROPY)"

# ---- 2. the Keel mbedTLS adapter + mbedTLS platform TUs ----
ADAPTER_CFLAGS=(
  "${BASE_TARGET[@]}"
  -DKEEL_FREESTANDING
  "${MBEDTLS_DEFS[@]}"
  "${ENTROPY_DEFS[@]}"
  -I"$MBEDTLS_ADAPTER"
  -I"$MBEDTLS_SRC/include"
  -I"$KEEL_ROOT/include" -I"$KEEL_ROOT/src"
  -I.
  -isystem "$LOCAL_SHIM"
  -isystem "$SHIM"
  -w
)
for t in tls_mbedtls:"$MBEDTLS_ADAPTER/tls_mbedtls.c" \
         mbedtls_platform_uefi:mbedtls_platform_uefi.c \
         entropy_uefi:entropy_uefi.c \
         time_uefi:time_uefi.c; do
  name="${t%%:*}"; src="${t#*:}"
  echo "compiling $name ..."
  "$CC" "${ADAPTER_CFLAGS[@]}" -c -o "$name.obj" "$src"
  OBJS+=( "$name.obj" )
done

# ---- 3. U-1/U-2/U-3 Keel UEFI TUs + the link residuals + s6_selftest.c ----
KEEL_CFLAGS=(
  "${BASE_TARGET[@]}"
  -DKEEL_FREESTANDING
  -DKL_UEFI_ASSUME_UNSPECIFIED_UTC
  -I"$KEEL_ROOT/include" -I"$KEEL_ROOT/vendor/llhttp" -I"$KEEL_ROOT/src"
  -I"$MBEDTLS_ADAPTER"           # s6_selftest.c includes keel_tls_mbedtls.h
  -I.                            # s6_cert_embed.h
  -isystem "$SHIM"
  -Wall -Wextra
)
# NO resolve_uefi.c (server does not resolve); u1_link_stubs.c WITHOUT
# -DKEEL_UEFI_HAVE_RESOLVE so its fail-closed kl_resolve_sync stands (unused). Plus
# s4_link_stubs.c for the server-only seam residuals (bind/listen/accept/... + _fltused).
KEEL_SRCS=( allocator_uefi.c errno_uefi.c platform_uefi.c civil_time.c wallclock_uefi.c clock_snapshot.c
            socket_efi_tcp4.c event_efi.c u1_link_stubs.c s4_link_stubs.c s6_selftest.c )
for s in "${KEEL_SRCS[@]}"; do
  o="${s%.c}.obj"
  echo "compiling $s ..."
  "$CC" "${KEEL_CFLAGS[@]}" -c -o "$o" "$s"
  OBJS+=( "$o" )
done

# ---- 4. link ----
echo "linking BOOTX64.EFI (lld PE, subsystem=efi_application, -nostdlib, gc-sections)..."
"$CC" --target=x86_64-unknown-windows \
  -ffreestanding -fno-stack-protector -fno-builtin \
  -nostdlib -fuse-ld=lld \
  -Wl,-entry:efi_main -Wl,-subsystem:efi_application \
  -Wl,-opt:ref \
  -o BOOTX64.EFI "${OBJS[@]}" "$ARCHIVE"

echo "built: $(stat -c%s BOOTX64.EFI 2>/dev/null || stat -f%z BOOTX64.EFI) bytes -> BOOTX64.EFI"
command -v file >/dev/null 2>&1 && file BOOTX64.EFI || true
