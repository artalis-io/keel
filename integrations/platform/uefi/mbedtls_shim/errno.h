/*
 * mbedtls_shim/errno.h — freestanding <errno.h> for the mbedTLS TUs.
 *
 * tls_mbedtls.c's bio_send/bio_recv reference errno + EAGAIN/EWOULDBLOCK, but on
 * the EFI target that BIO runs in memory/socket-pump mode and NEVER hits the
 * would-block branch (the socket provider is a synchronous pump). So this only
 * needs to make the code COMPILE — the value is irrelevant. `errno` is defined as a
 * real global in mbedtls_platform_uefi.c.
 *
 * Kept separate from tests/freestanding/shim/errno.h (which is also on the include
 * path) — first-found on the -isystem search order wins; both define the same
 * guard-free macros identically, so either is fine.
 */
#ifndef KEEL_UEFI_MBEDTLS_SHIM_ERRNO_H
#define KEEL_UEFI_MBEDTLS_SHIM_ERRNO_H
extern int errno;
#ifndef EAGAIN
#define EAGAIN      11
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK 11
#endif
#ifndef EINTR
#define EINTR        4
#endif
#endif /* KEEL_UEFI_MBEDTLS_SHIM_ERRNO_H */
