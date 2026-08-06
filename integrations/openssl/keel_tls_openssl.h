/*
 * keel_tls_openssl.h — OpenSSL / BoringSSL backend for Keel's KlTls vtable
 *
 * Provides TLS 1.2/1.3 server (and client) support using OpenSSL 3.x or
 * BoringSSL. The SAME adapter TU (tls_openssl.c) serves both libraries — the
 * few API divergences are guarded with `#if defined(OPENSSL_IS_BORINGSSL)`.
 * Supports mutual TLS (mTLS) with configurable client authentication and both
 * the readiness (socket-BIO) and completion (memory-BIO) transport modes.
 *
 * Usage (server):
 *   KlAllocator alloc = kl_allocator_default();
 *   KlTlsCtx *ctx = kl_tls_openssl_ctx_create("cert.pem", "key.pem",
 *                                              NULL, KL_MTLS_NONE, &alloc);
 *   KlConfig config = {
 *       .tls = &(KlTlsConfig){
 *           .ctx = ctx,
 *           .factory = kl_tls_openssl_create,
 *           .ctx_destroy = kl_tls_openssl_ctx_destroy,
 *       },
 *       ...
 *   };
 *
 * Usage (client):
 *   KlTlsCtx *ctx = kl_tls_openssl_client_ctx_create(NULL, &alloc);
 *   KlTls *tls = kl_tls_openssl_create(ctx, &alloc);
 *   kl_tls_openssl_set_hostname(tls, "example.com");
 *   tls->handshake(tls, fd);  tls->write(...);  tls->read(...);
 *   tls->shutdown(tls, fd);   tls->destroy(tls);
 *   kl_tls_openssl_ctx_destroy(ctx);
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KEEL_TLS_OPENSSL_H
#define KEEL_TLS_OPENSSL_H

#include <keel/tls.h>
#include <keel/allocator.h>

/*
 * LIFECYCLE: a KlTlsCtx is a LONG-LIVED, reusable object — create it ONCE (per
 * server, or per client configuration/connection-pool) and make many KlTls sessions
 * from it via the factory. Do NOT create and destroy a context per request. Besides
 * the cost of rebuilding the SSL_CTX, each context allocates its own custom
 * BIO_METHOD via BIO_get_new_index(), whose process-global type index is not
 * recycled on destroy; churning contexts per request would slowly consume indices.
 * The intended usage (one context, many sessions) has no such concern.
 */

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
 * @param client_auth Client authentication mode (KlMtlsMode). When OPTIONAL or
 *                    REQUIRED, ca_path must name a non-empty CA bundle (else the
 *                    call fails). An out-of-range mode value is rejected.
 * @param alloc       Allocator for context storage (borrowed — must outlive context).
 * @return Opaque context, or NULL on error.
 */
KlTlsCtx *kl_tls_openssl_ctx_create(const char *cert_path,
                                    const char *key_path,
                                    const char *ca_path,
                                    KlMtlsMode client_auth,
                                    KlAllocator *alloc);

/**
 * @brief Create a server TLS context from in-memory cert/key/(optional CA).
 *
 * Same as kl_tls_openssl_ctx_create() but reads cert, key, and (optional) mTLS
 * CA from PEM buffers instead of files. Buffers are parsed and copied
 * internally; the caller may free them immediately after this returns.
 *
 * @param cert_buf    Server cert bytes (PEM). Must be non-NULL.
 * @param cert_len    Length of cert_buf.
 * @param key_buf     Private key bytes (PEM). Must be non-NULL.
 * @param key_len     Length of key_buf.
 * @param ca_buf      mTLS CA bytes (PEM), or NULL to disable client authentication.
 *                    Required (non-NULL, non-empty) when client_auth is OPTIONAL
 *                    or REQUIRED. ca_buf/ca_len must agree (both set or both zero).
 * @param ca_len      CA length (0 when ca_buf is NULL).
 * @param client_auth Client authentication mode (KlMtlsMode). Out-of-range values
 *                    are rejected.
 * @param alloc       Allocator for context storage (borrowed — must outlive it).
 * @return Opaque context, or NULL on error.
 */
KlTlsCtx *kl_tls_openssl_ctx_create_from_buf(const unsigned char *cert_buf, size_t cert_len,
                                             const unsigned char *key_buf, size_t key_len,
                                             const unsigned char *ca_buf, size_t ca_len,
                                             KlMtlsMode client_auth, KlAllocator *alloc);

/**
 * @brief Create a client-side TLS context (for outbound connections).
 *
 * Verification is ON by default (SSL_VERIFY_PEER + hostname matching via
 * kl_tls_openssl_set_hostname()).
 *
 * @param ca_path  Path to PEM-encoded CA cert bundle for server verification.
 *                 NULL uses the system default trust store
 *                 (SSL_CTX_set_default_verify_paths) — still verifying. To skip
 *                 verification entirely (tests / cert-pinning done elsewhere),
 *                 use kl_tls_openssl_client_ctx_create_insecure().
 * @param alloc    Allocator for context storage (borrowed — must outlive context).
 * @return Opaque context, or NULL on error.
 */
KlTlsCtx *kl_tls_openssl_client_ctx_create(const char *ca_path,
                                           KlAllocator *alloc);

/**
 * @brief Create a TLS client context from an in-memory PEM CA bundle.
 *
 * Same as kl_tls_openssl_client_ctx_create() but reads the CA bundle from a
 * buffer. The buffer is parsed and copied internally; the caller may free it
 * immediately after this call returns. Verification is ON.
 *
 * @param ca_buf  PEM CA bundle bytes. NULL uses the system default trust store
 *                (still verifying); to disable verification use
 *                kl_tls_openssl_client_ctx_create_insecure().
 * @param ca_len  Length in bytes (0 when ca_buf is NULL).
 * @param alloc   Allocator for context storage (borrowed — must outlive context).
 * @return Opaque context, or NULL on error.
 */
KlTlsCtx *kl_tls_openssl_client_ctx_create_from_buf(const unsigned char *ca_buf,
                                                    size_t ca_len,
                                                    KlAllocator *alloc);

/**
 * @brief Create a client TLS context with certificate verification DISABLED.
 *
 * WARNING: verify-none. The connection is encrypted but MITM-vulnerable — the
 * server certificate is NOT checked against any CA and the hostname is NOT
 * matched. Use ONLY for tests or when peer authenticity is guaranteed by some
 * other means (e.g. certificate pinning performed by the caller). Production
 * code must use kl_tls_openssl_client_ctx_create() / _from_buf() instead.
 *
 * @param alloc  Allocator for context storage (borrowed — must outlive context).
 * @return Opaque context, or NULL on error.
 */
KlTlsCtx *kl_tls_openssl_client_ctx_create_insecure(KlAllocator *alloc);

/**
 * @brief Attach a client certificate + key to a client context (mTLS).
 *
 * For outbound connections to a server that requires client authentication.
 * Load the client leaf cert (PEM, optionally with chain) and its private key.
 * Call on a context from kl_tls_openssl_client_ctx_create*(), before creating
 * any session. Buffers are parsed internally; the caller may free them after.
 *
 * @param ctx      Client context.
 * @param cert_buf Client cert PEM. Must be non-NULL.
 * @param cert_len Length of cert_buf.
 * @param key_buf  Client private key PEM. Must be non-NULL.
 * @param key_len  Length of key_buf.
 * @return 0 on success, -1 on error.
 */
int kl_tls_openssl_client_ctx_set_cert(KlTlsCtx *ctx,
                                       const unsigned char *cert_buf, size_t cert_len,
                                       const unsigned char *key_buf, size_t key_len);

/**
 * @brief Destroy a TLS context. Safe to pass as ctx_destroy in KlTlsConfig.
 */
void kl_tls_openssl_ctx_destroy(KlTlsCtx *ctx);

/**
 * @brief Factory: create a per-connection KlTls session (server or client ctx).
 *        Pass as the `factory` field in KlTlsConfig.
 *
 * @param ctx   Shared context from one of the ctx constructors above.
 * @param alloc Allocator for session resources.
 * @return New TLS session, or NULL on failure.
 */
KlTls *kl_tls_openssl_create(KlTlsCtx *ctx, KlAllocator *alloc);

/**
 * @brief Set the expected server hostname for SNI + verification (client mode).
 *
 * Sets the SNI server_name and, when the client ctx verifies, enables OpenSSL
 * hostname matching (SSL_set1_host / X509_VERIFY_PARAM). Call after
 * kl_tls_openssl_create() and before handshake().
 *
 * @param tls      TLS session from kl_tls_openssl_create().
 * @param hostname Server hostname.
 * @return 0 on success, -1 on error.
 */
int kl_tls_openssl_set_hostname(KlTls *tls, const char *hostname);

/**
 * @brief Configure the ALPN protocol list for a context (server or client).
 *
 * Client context: protocols offered in the ClientHello (SSL_CTX_set_alpn_protos).
 * Server context: protocols the server will select from via an ALPN select
 * callback (server picks its own first entry that the client also offered).
 *
 * Pass a NULL-terminated array, most-preferred first, e.g.
 * `const char *p[] = {"h2","http/1.1",NULL};`. Strings are copied into the ctx.
 *
 * @param ctx    Server or client context.
 * @param protos NULL-terminated, preference-ordered protocol names.
 * @return 0 on success, -1 on error.
 */
int kl_tls_openssl_ctx_set_alpn(KlTlsCtx *ctx, const char **protos);

/**
 * @brief Route the TLS transport's socket I/O through a KlSocketProvider.
 *
 * By default the socket-BIO does ciphertext send/recv through the built-in host
 * socket ops (kl_sockdef_*). Set a provider here to run TLS over a non-kernel
 * stack (e.g. lwIP). Passing NULL restores the host default. Applies only to the
 * synchronous socket-BIO path; the completion (memory-BIO) mode ignores it.
 * Optional — usually the framework auto-wires this via KlTls.set_socket_provider.
 *
 * @param ctx Server or client context.
 * @param sp  Socket provider to route through, or NULL for the host default.
 * @return 0 on success, -1 on error (NULL ctx).
 */
struct KlSocketProvider;
int kl_tls_openssl_ctx_set_socket_provider(KlTlsCtx *ctx,
                                           const struct KlSocketProvider *sp);

/**
 * @brief Allow a bare transport EOF (no TLS close_notify) to count as a clean EOF.
 *
 * By default (allow == 0) the adapter is STRICT: only a real close_notify
 * (SSL_ERROR_ZERO_RETURN) marks at_eof(); a transport EOF without close_notify is
 * an error (read() returns -1 and at_eof() stays 0). This prevents a truncated
 * close-delimited HTTPS response from being silently accepted as complete.
 *
 * Setting allow != 0 restores the lenient behavior: a clean transport EOF with no
 * queued TLS error is treated as at_eof(). Sessions inherit this from the context
 * at creation. Applies to the readiness (socket-BIO) path.
 *
 * @param ctx    Server or client context.
 * @param allow  0 = strict (default), non-zero = allow truncation.
 * @return 0 on success, -1 on error (NULL ctx).
 */
int kl_tls_openssl_ctx_set_allow_truncation(KlTlsCtx *ctx, int allow);

#endif /* KEEL_TLS_OPENSSL_H */
