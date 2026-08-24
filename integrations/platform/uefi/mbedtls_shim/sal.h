/*
 * mbedtls_shim/sal.h: empty MSVC SAL-annotation shim for the mbedTLS build.
 *
 * clang --target=x86_64-unknown-windows defines _MSC_VER (MSVC compat) but NOT
 * __GNUC__, so mbedtls/platform_util.h takes its _MSC_VER branch and pulls <sal.h>
 * for the _Check_return_ source annotation. sal.h is a Windows-SDK header, absent
 * freestanding. The annotations are advisory only (no codegen effect), so an empty
 * header + a no-op _Check_return_ is fully sufficient.
 */
#ifndef KEEL_UEFI_MBEDTLS_SHIM_SAL_H
#define KEEL_UEFI_MBEDTLS_SHIM_SAL_H
#ifndef _Check_return_
#define _Check_return_
#endif
#endif /* KEEL_UEFI_MBEDTLS_SHIM_SAL_H */
