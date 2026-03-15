/*
 * tls_mbedtls.h — mbedTLS backend for Keel's KlTls vtable
 *
 * Provides TLS 1.2/1.3 server (and client) support using mbedTLS.
 * Supports mutual TLS (mTLS) with configurable client authentication.
 *
 * Usage:
 *   KlAllocator alloc = kl_allocator_default();
 *   KlTlsCtx *ctx = kl_tls_mbedtls_ctx_create("cert.pem", "key.pem",
 *                                               NULL, 0, &alloc);
 *   KlConfig config = {
 *       .tls = &(KlTlsConfig){
 *           .ctx = ctx,
 *           .factory = kl_tls_mbedtls_create,
 *           .ctx_destroy = kl_tls_mbedtls_ctx_destroy,
 *       },
 *       ...
 *   };
 *
 * For client-side usage (e.g. Hull HTTP client):
 *   KlAllocator alloc = kl_allocator_default();
 *   KlTlsCtx *ctx = kl_tls_mbedtls_client_ctx_create(NULL, &alloc);
 *   KlTls *tls = kl_tls_mbedtls_create(ctx, &alloc);
 *   tls->handshake(tls, fd);
 *   tls->write(tls, fd, buf, len);
 *   tls->read(tls, fd, buf, len);
 *   tls->shutdown(tls, fd);
 *   tls->destroy(tls);
 *   kl_tls_mbedtls_ctx_destroy(ctx);
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KEEL_TLS_MBEDTLS_H
#define KEEL_TLS_MBEDTLS_H

#include <keel/tls.h>
#include <keel/allocator.h>

/**
 * @brief Client authentication mode for mTLS.
 */
typedef enum {
    KL_MTLS_NONE     = 0,  /**< No client certificate requested */
    KL_MTLS_OPTIONAL = 1,  /**< Request cert, allow unauthenticated */
    KL_MTLS_REQUIRED = 2   /**< Require valid client certificate */
} KlMtlsMode;

/**
 * @brief Create a server-side TLS context (certificates + keys).
 *
 * @param cert_path   Path to PEM-encoded server certificate (chain).
 * @param key_path    Path to PEM-encoded server private key.
 * @param ca_path     Path to PEM-encoded CA cert for client verification (mTLS).
 *                    NULL to disable client authentication.
 * @param client_auth Client authentication mode (KlMtlsMode).
 * @param alloc       Allocator for context storage (borrowed — must outlive context).
 * @return Opaque context, or NULL on error.
 */
KlTlsCtx *kl_tls_mbedtls_ctx_create(const char *cert_path,
                                      const char *key_path,
                                      const char *ca_path,
                                      int client_auth,
                                      KlAllocator *alloc);

/**
 * @brief Create a client-side TLS context (for outbound connections).
 *
 * @param ca_path  Path to PEM-encoded CA cert bundle for server verification.
 *                 NULL skips certificate verification — requires explicit
 *                 opt-in via --skip-ca-bundle flag. Production deployments
 *                 should always provide a valid CA bundle path.
 * @param alloc    Allocator for context storage (borrowed — must outlive context).
 * @return Opaque context, or NULL on error.
 */
KlTlsCtx *kl_tls_mbedtls_client_ctx_create(const char *ca_path,
                                              KlAllocator *alloc);

/**
 * @brief Destroy a TLS context.
 *
 * Safe to pass as ctx_destroy in KlTlsConfig.
 */
void kl_tls_mbedtls_ctx_destroy(KlTlsCtx *ctx);

/**
 * @brief Factory: create a per-connection KlTls session.
 *
 * Works for both server-side and client-side contexts.
 * Pass as the `factory` field in KlTlsConfig.
 *
 * @param ctx   Shared context from kl_tls_mbedtls_ctx_create() or
 *              kl_tls_mbedtls_client_ctx_create().
 * @param alloc Allocator for session resources.
 * @return New TLS session, or NULL on failure.
 */
KlTls *kl_tls_mbedtls_create(KlTlsCtx *ctx, KlAllocator *alloc);

/**
 * @brief Set the expected server hostname for SNI (client mode).
 *
 * Must be called after kl_tls_mbedtls_create() and before handshake().
 *
 * @param tls      TLS session from kl_tls_mbedtls_create().
 * @param hostname Server hostname for SNI and certificate verification.
 * @return 0 on success, -1 on error.
 */
int kl_tls_mbedtls_set_hostname(KlTls *tls, const char *hostname);

#endif /* KEEL_TLS_MBEDTLS_H */
