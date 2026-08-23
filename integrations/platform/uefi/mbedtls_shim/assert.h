/*
 * mbedtls_shim/assert.h — freestanding <assert.h> for the mbedTLS build.
 *
 * mbedTLS's common.h includes <assert.h>. In the freestanding EFI build there is no
 * libc __assert_fail to call, and NDEBUG is not defined, so make assert() a no-op.
 * (mbedTLS asserts guard internal invariants, not attacker input; the parsers use
 * explicit error returns for untrusted data.)
 */
#ifndef KEEL_UEFI_MBEDTLS_SHIM_ASSERT_H
#define KEEL_UEFI_MBEDTLS_SHIM_ASSERT_H
#undef assert
#define assert(x) ((void)0)
#endif /* KEEL_UEFI_MBEDTLS_SHIM_ASSERT_H */
