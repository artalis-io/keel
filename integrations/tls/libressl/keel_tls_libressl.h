/*
 * keel_tls_libressl.h — LibreSSL backend for Keel's KlTls vtable.
 *
 * LibreSSL is API-compatible enough with OpenSSL that the SAME adapter TU
 * (integrations/tls/openssl/tls_openssl.c) serves it too. There is therefore NO
 * separate LibreSSL source: this header re-exports the OpenSSL adapter's public
 * API. The `kl_tls_openssl_*` names denote the adapter family, not the linked
 * library.
 *
 * The adapter guards the few LibreSSL/OpenSSL divergences with
 * `#if defined(LIBRESSL_VERSION_NUMBER)` (a macro LibreSSL's <openssl/opensslv.h>
 * defines) — currently just SSL_get_peer_certificate vs OpenSSL 3.x's
 * SSL_get1_peer_certificate. Build integrations/tls/openssl/tls_openssl.c against a
 * LibreSSL prefix via this directory's Makefile (LIBRESSL_DIR=...).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KEEL_TLS_LIBRESSL_H
#define KEEL_TLS_LIBRESSL_H

/* The LibreSSL backend IS the OpenSSL adapter compiled against LibreSSL. */
#include "../openssl/keel_tls_openssl.h"

#endif /* KEEL_TLS_LIBRESSL_H */
