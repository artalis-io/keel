/*
 * keel_tls_boringssl.h: BoringSSL backend for Keel's KlTls vtable.
 *
 * BoringSSL is API-compatible enough with OpenSSL that the SAME adapter TU
 * (integrations/tls/openssl/tls_openssl.c) serves both. There is therefore NO
 * separate BoringSSL source: this header simply re-exports the OpenSSL adapter's
 * public API. The `kl_tls_openssl_*` names are used for both libraries; the name
 * denotes the adapter family, not the linked library.
 *
 * The adapter guards the few BoringSSL/OpenSSL divergences with
 * `#if defined(OPENSSL_IS_BORINGSSL)` (a macro BoringSSL's <openssl/base.h>
 * defines). Build integrations/tls/openssl/tls_openssl.c against a BoringSSL prefix
 * via this directory's Makefile (BORINGSSL_DIR=...).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KEEL_TLS_BORINGSSL_H
#define KEEL_TLS_BORINGSSL_H

/* The BoringSSL backend IS the OpenSSL adapter compiled against BoringSSL. */
#include "../openssl/keel_tls_openssl.h"

#endif /* KEEL_TLS_BORINGSSL_H */
