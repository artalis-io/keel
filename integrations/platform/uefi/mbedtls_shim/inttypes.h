/*
 * mbedtls_shim/inttypes.h: freestanding <inttypes.h> for the mbedTLS build.
 *
 * clang ships its own <inttypes.h> that #include_next's the libc one, which is
 * absent freestanding. mbedTLS (ssl.h) pulls <inttypes.h> for the fixed-width PRI*
 * format macros. We provide <stdint.h> (clang freestanding builtin) plus the PRI*
 * macros mbedTLS may reference. This shim is searched via -isystem BEFORE clang's
 * own inttypes.h, so it wins and never triggers the include_next.
 */
#ifndef KEEL_UEFI_MBEDTLS_SHIM_INTTYPES_H
#define KEEL_UEFI_MBEDTLS_SHIM_INTTYPES_H

#include <stdint.h>

/* Fixed-width printf/scanf length modifiers (LP64 / MS x64: 64-bit = "ll"). */
#define PRId8   "d"
#define PRId16  "d"
#define PRId32  "d"
#define PRId64  "lld"
#define PRIi8   "i"
#define PRIi16  "i"
#define PRIi32  "i"
#define PRIi64  "lli"
#define PRIu8   "u"
#define PRIu16  "u"
#define PRIu32  "u"
#define PRIu64  "llu"
#define PRIx8   "x"
#define PRIx16  "x"
#define PRIx32  "x"
#define PRIx64  "llx"
#define PRIX8   "X"
#define PRIX16  "X"
#define PRIX32  "X"
#define PRIX64  "llX"
#define PRIdPTR "lld"
#define PRIuPTR "llu"
#define PRIxPTR "llx"

#endif /* KEEL_UEFI_MBEDTLS_SHIM_INTTYPES_H */
