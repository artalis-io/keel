#!/usr/bin/env bash
# build_u4.sh: build the U-4 KlHttpClient HTTPS-over-EFI_TCP4+mbedTLS self-test into
# BOOTX64.EFI.
#
# Extends build_u3.sh with a FREESTANDING mbedTLS build:
#   - compile ALL mbedTLS library/*.c with the EFI freestanding CFLAGS + the U-4
#     config (mbedtls_config_uefi.h), warnings relaxed (-w, like vendor code).
#   - compile the Keel mbedTLS adapter (integrations/tls/mbedtls/tls_mbedtls.c) + the
#     U-4 platform TU (mbedtls_platform_uefi.c: EFI heap/entropy + libc residuals).
#   - the U-1/U-2/U-3 TUs (allocator/platform/socket/event/resolve/link-stubs).
#   - u4_selftest.c (entry).
#   - link with the freestanding self-contained archive via lld PE -> BOOTX64.EFI.
#
# Requires MBEDTLS_SRC to point at an extracted mbedTLS RELEASE tarball tree
# (with library/*.c + include/). run_u4.sh downloads + sets it.
#
# Env:
#   CC           clang (default)
#   KEEL_ROOT    repo root holding the freestanding archive (default: ../../../..)
#   ARCHIVE      override the freestanding archive path
#   MBEDTLS_SRC  extracted mbedTLS release tarball tree (REQUIRED)
#   TARGET_PORT  responder port compiled into the URL (default 18443)
#   TARGET_HOST  responder host compiled into the URL (default 10.0.2.2)
set -euo pipefail
if [ "${BASH_VERSINFO:-0}" -lt 4 ]; then echo "ERROR: this script needs bash 4+ (uses associative arrays); on macOS run: brew install bash, or run it in the container." >&2; exit 3; fi
cd "$(dirname "$0")"

: "${CC:=clang}"
: "${KEEL_ROOT:=../../../..}"
: "${TARGET_PORT:=18443}"
: "${TARGET_HOST:=10.0.2.2}"
: "${MBEDTLS_SRC:?set MBEDTLS_SRC to an extracted mbedTLS release tarball tree (include/ + library/)}"

if [ ! -d "$MBEDTLS_SRC/library" ] || [ ! -d "$MBEDTLS_SRC/include" ]; then
  echo "ERROR: MBEDTLS_SRC=$MBEDTLS_SRC is missing library/ or include/" >&2
  exit 2
fi

SHIM="$KEEL_ROOT/tests/freestanding/shim"
LOCAL_SHIM="../mbedtls_shim"
MBEDTLS_ADAPTER="$KEEL_ROOT/integrations/tls/mbedtls"
: "${ARCHIVE:=$KEEL_ROOT/libkeel_freestanding_selfcontained_x86_64.a}"
if [ ! -f "$ARCHIVE" ]; then ARCHIVE="$KEEL_ROOT/libkeel_freestanding_selfcontained.a"; fi
if [ ! -f "$ARCHIVE" ]; then
  echo "ERROR: freestanding archive not found. Run 'make freestanding-lib-selfcontained' in $KEEL_ROOT first." >&2
  exit 2
fi
echo "using archive:     $ARCHIVE"
echo "using mbedtls src: $MBEDTLS_SRC"

# Common freestanding EFI target flags (shared by all TUs).
# -fno-autolink: the windows target sets _MSC_VER, so mbedTLS's x509_crt.c emits
# `#pragma comment(lib, "ws2_32.lib")` which lld-link would try to resolve (and fail,
# freestanding). We never call the ws2_32 inet_pton (MBEDTLS_TEST_SW_INET_PTON forces
# the bundled impl), so drop the autolink directive entirely.
BASE_TARGET=(
  --target=x86_64-unknown-windows
  -ffreestanding -fshort-wchar
  -fno-stack-protector -fno-builtin -fno-autolink
  -mno-red-zone -std=c11
)

# The config macro must reach every mbedTLS TU + tls_mbedtls.c so both agree on the
# compiled feature set. The config file itself lives in this dir.
MBEDTLS_DEFS=(
  -DMBEDTLS_CONFIG_FILE='"mbedtls_config_uefi.h"'
  # Force mbedTLS's OWN software inet_pton in x509_crt.c: the clang windows target
  # sets _MSC_VER, steering x509_crt.c into the MSVC branch that would need a real
  # ws2_32 inet_pton. This bypass uses the bundled portable impl instead (no libc).
  -DMBEDTLS_TEST_SW_INET_PTON
)
MBEDTLS_INCS=(
  -I"$MBEDTLS_SRC/include"
  -I. -I..                          # mbedtls_config_uefi.h
  -isystem "$LOCAL_SHIM"       # mbedTLS-specific: ws2tcpip (INET6_ADDRSTRLEN/inet_ntop), errno
  -isystem "$SHIM"             # base freestanding shim: string/stdlib/stdio/winsock2/...
)

OBJS=()

# ---- 1. mbedTLS library/*.c (warnings relaxed like vendor code) ----
echo "compiling mbedTLS library/*.c (freestanding, -w) ..."
mkdir -p mbedtls_obj
for src in "$MBEDTLS_SRC"/library/*.c; do
  base="$(basename "$src" .c)"
  o="mbedtls_obj/${base}.obj"
  "$CC" "${BASE_TARGET[@]}" "${MBEDTLS_DEFS[@]}" "${MBEDTLS_INCS[@]}" \
    -ffunction-sections -fdata-sections -w -c -o "$o" "$src"
  OBJS+=( "$o" )
done
echo "  compiled $(ls mbedtls_obj/*.obj | wc -l) mbedTLS objects"

# F4: entropy policy. SPIKE mode (default: verify-none, no virtio-rng) opts into the
# INSECURE weak-entropy fallback so the demo can proceed over weak entropy (matching the
# loud runtime warning). PROD mode (U4_MODE=prod) does NOT define it; it requires real
# EFI_RNG via virtio-rng, and mbedtls_hardware_poll fails closed without it.
ENTROPY_DEFS=()
if [ "${U4_MODE:-spike}" != "prod" ]; then
  ENTROPY_DEFS=( -DKL_UEFI_INSECURE_TEST_ENTROPY )
  echo "entropy policy:    INSECURE test fallback (spike; -DKL_UEFI_INSECURE_TEST_ENTROPY)"
else
  echo "entropy policy:    FAIL-CLOSED (prod; real EFI_RNG required)"
fi

# ---- 2. the Keel mbedTLS adapter + U-4 platform TU ----
# tls_mbedtls.c needs the adapter header + src/ (socket.h) + mbedtls include.
ADAPTER_CFLAGS=(
  "${BASE_TARGET[@]}"
  -DKEEL_FREESTANDING
  "${MBEDTLS_DEFS[@]}"
  "${ENTROPY_DEFS[@]}"            # F4: spike → insecure fallback; prod → fail-closed
  -I"$MBEDTLS_ADAPTER"
  -I"$MBEDTLS_SRC/include"
  -I"$KEEL_ROOT/include" -I"$KEEL_ROOT/src"
  -I. -I..
  -isystem "$LOCAL_SHIM"
  -isystem "$SHIM"
  -w                              # adapter is BYO/vendor-ish here; relax under freestanding
)
echo "compiling tls_mbedtls.c ..."
"$CC" "${ADAPTER_CFLAGS[@]}" -c -o tls_mbedtls.obj "$MBEDTLS_ADAPTER/tls_mbedtls.c"
OBJS+=( tls_mbedtls.obj )

echo "compiling ../mbedtls_platform_uefi.c ..."
"$CC" "${ADAPTER_CFLAGS[@]}" -c -o mbedtls_platform_uefi.obj ../mbedtls_platform_uefi.c
OBJS+=( mbedtls_platform_uefi.obj )

# entropy_uefi.c = mbedtls_hardware_poll, split out of the platform TU so the host mock
# harness can test the fail-closed contract without the libc-clashing residuals. Needs the
# entropy defines (spike → insecure fallback; prod → fail-closed) but NOT the mbedTLS include.
echo "compiling ../entropy_uefi.c ..."
"$CC" "${ADAPTER_CFLAGS[@]}" -c -o entropy_uefi.obj ../entropy_uefi.c
OBJS+=( entropy_uefi.obj )

# time_uefi.c = the mbedTLS cert-validity clock hooks (kl_uefi_mbedtls_time +
# mbedtls_platform_gmtime_r) over Runtime Services GetTime. Needs struct tm from the local
# mbedtls_shim <time.h>, so it uses the ADAPTER_CFLAGS (which carry -isystem LOCAL_SHIM).
echo "compiling ../time_uefi.c ..."
"$CC" "${ADAPTER_CFLAGS[@]}" -c -o time_uefi.obj ../time_uefi.c
OBJS+=( time_uefi.obj )

# ---- 3. U-1/U-2/U-3 Keel UEFI TUs + u4_selftest.c ----
# U4_MODE=prod: production TLS, CA-verified server cert (dNSName SAN = KL_U4_PROD_HOST,
# mapped to KL_U4_STATIC_IP by resolve_uefi.c's static hosts entry) + real EFI_RNG
# (fail-closed). The CA is embedded from u4_ca_embed.h (generated by run_u4.sh). Default
# (spike) = verify-none + weak-entropy fallback.
PROD_DEFS=()
if [ "${U4_MODE:-spike}" = "prod" ]; then
  : "${KL_U4_PROD_HOST:=server.keel.test}"
  : "${KL_U4_STATIC_IP:=10.0.2.2}"
  [ -f u4_ca_embed.h ] || { echo "ERROR: U4_MODE=prod needs u4_ca_embed.h (run_u4.sh generates it)"; exit 2; }
  PROD_DEFS=(
    -DKL_U4_PROD
    -DKL_U4_PROD_HOST="\"$KL_U4_PROD_HOST\""
    -DKL_U4_STATIC_HOST="\"$KL_U4_PROD_HOST\""
    -DKL_U4_STATIC_IP="\"$KL_U4_STATIC_IP\""
    -I. -I..                              # u4_ca_embed.h lives in this dir
  )
fi
KEEL_CFLAGS=(
  "${BASE_TARGET[@]}"       # includes -fno-autolink
  -DKEEL_FREESTANDING
  -DTARGET_PORT="$TARGET_PORT"
  -DTARGET_HOST="\"$TARGET_HOST\""
  # OVMF/QEMU reports UTC with an UNSPECIFIED timezone; this TEST/OVMF flag lets the cert clock
  # treat an unspecified zone as already-UTC. It is a test accommodation; a real-firmware
  # production build omits it, so unspecified-zone (local) time is rejected unless the app
  # declares an offset via kl_uefi_set_unspecified_tz(). See wallclock_uefi.h.
  -DKL_UEFI_ASSUME_UNSPECIFIED_UTC
  "${PROD_DEFS[@]}"
  -I"$KEEL_ROOT/include" -I"$KEEL_ROOT/vendor/llhttp" -I"$KEEL_ROOT/src"
  -I"$MBEDTLS_ADAPTER"            # u4_selftest.c includes keel_tls_mbedtls.h
  -isystem "$SHIM"
  -Wall -Wextra
)
declare -A EXTRA=( [u1_link_stubs.c]="-DKEEL_UEFI_HAVE_RESOLVE"
                   [u4_selftest.c]="${U4_SELFTEST_DEFS:-}" )
KEEL_SRCS=( ../allocator_uefi.c ../errno_uefi.c ../platform_uefi.c ../civil_time.c ../wallclock_uefi.c ../clock_snapshot.c
            ../socket_efi_tcp4.c ../event_efi.c ../resolve_uefi.c u1_link_stubs.c u4_selftest.c )
for s in "${KEEL_SRCS[@]}"; do
  o="$(basename "${s%.c}").obj"
  echo "compiling $s ..."
  "$CC" "${KEEL_CFLAGS[@]}" ${EXTRA[$s]:-} -c -o "$o" "$s"
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
file BOOTX64.EFI || true
