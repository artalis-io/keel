/*
 * mbedtls_shim/windows.h: minimal <windows.h> for the mbedTLS build.
 *
 * clang --target=x86_64-unknown-windows defines _WIN32, so mbedTLS's
 * platform_util.c takes `#if defined(_WIN32) #include <windows.h>`, solely to use
 * SecureZeroMemory() for its secure-zeroize routine. Freestanding has no Windows
 * SDK, so we provide just SecureZeroMemory as a volatile byte-store loop (the same
 * optimization-safe pattern mbedTLS's fallback uses). Nothing else from windows.h
 * is referenced by the compiled TU set.
 */
#ifndef KEEL_UEFI_MBEDTLS_SHIM_WINDOWS_H
#define KEEL_UEFI_MBEDTLS_SHIM_WINDOWS_H

#include <stddef.h>

static inline void SecureZeroMemory(void *ptr, size_t cnt) {
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    while (cnt--) *p++ = 0;
}

#endif /* KEEL_UEFI_MBEDTLS_SHIM_WINDOWS_H */
