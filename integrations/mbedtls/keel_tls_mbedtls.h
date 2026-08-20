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
 *   KlHttpServerConfig config = {
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
 * @brief Create a server TLS context from in-memory cert/key/(optional CA).
 *
 * Same as kl_tls_mbedtls_ctx_create() but reads cert, key, and (optional) mTLS
 * CA from buffers instead of files — for embedded certs where there is no
 * filesystem path. Buffers are parsed and copied internally; the caller may free
 * them immediately after this returns.
 *
 * @param cert_buf    Server cert bytes (PEM or DER). Must be non-NULL.
 * @param cert_len    Length; for PEM, must include the trailing NUL.
 * @param key_buf     Private key bytes (PEM or DER). Must be non-NULL.
 * @param key_len     Length; for PEM, must include the trailing NUL.
 * @param ca_buf      mTLS CA bytes, or NULL to disable client authentication.
 * @param ca_len      CA length (0 when ca_buf is NULL).
 * @param client_auth Client authentication mode (KlMtlsMode).
 * @param alloc       Allocator for context storage (borrowed — must outlive it).
 * @return Opaque context, or NULL on error.
 */
KlTlsCtx *kl_tls_mbedtls_ctx_create_from_buf(const unsigned char *cert_buf, size_t cert_len,
                                              const unsigned char *key_buf, size_t key_len,
                                              const unsigned char *ca_buf, size_t ca_len,
                                              int client_auth, KlAllocator *alloc);

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
 * @brief Create a TLS client context from an in-memory CA bundle.
 *
 * Same as kl_tls_mbedtls_client_ctx_create() but reads the CA bundle from
 * a buffer instead of a file. Useful for embedded bundles where there is
 * no filesystem path. The buffer must be PEM-encoded; binary DER is also
 * accepted by mbedTLS (it auto-detects).
 *
 * The buffer is parsed and copied internally; the caller may free it
 * immediately after this call returns.
 *
 * @param ca_buf  PEM (or DER) CA bundle bytes. Must be non-NULL.
 * @param ca_len  Length in bytes. For PEM, must include the trailing NUL
 *                — mbedTLS requires PEM input to be NUL-terminated.
 * @param alloc   Allocator for context storage (borrowed — must outlive context).
 * @return Opaque context, or NULL on error.
 */
KlTlsCtx *kl_tls_mbedtls_client_ctx_create_from_buf(const unsigned char *ca_buf,
                                                      size_t ca_len,
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

/**
 * @brief Configure the ALPN protocol list for a context (server or client).
 *
 * Server context: the protocols advertised in the TLS ServerHello ALPN
 * extension (mbedTLS selects the server's first entry that the client also
 * offers). Client context: the protocols offered in the ClientHello.
 *
 * Pass a NULL-terminated array, most-preferred first, e.g.
 * `const char *p[] = {"h2","http/1.1",NULL};`. The strings are copied into the
 * context, so the array need not outlive this call. Call once, after
 * kl_tls_mbedtls_ctx_create*() / _client_ctx_create*().
 *
 * The negotiated result is read back via the KlTls `alpn_protocol()` vtable
 * method after the handshake completes. See docs/alpn_policy.md.
 *
 * @param ctx    Server or client context.
 * @param protos NULL-terminated, preference-ordered protocol names.
 * @return 0 on success, -1 on error (NULL args, too many/long protocols).
 */
int kl_tls_mbedtls_ctx_set_alpn(KlTlsCtx *ctx, const char **protos);

/**
 * @brief Route the TLS transport's socket I/O through a KlSocketProvider.
 *
 * By default the socket-BIO does its ciphertext send/recv through the built-in
 * host socket ops (`kl_sockdef_*` — plain POSIX/Winsock). Set a provider here to
 * send/recv through it instead (`kl_sock_send`/`kl_sock_recv`), so TLS runs over a
 * non-kernel stack whose descriptors are not host fds — e.g. lwIP
 * (`kl_socket_provider_lwip()`), matching the socket/event providers the server or
 * client is already configured with. Passing NULL restores the default.
 *
 * Optional and usually unnecessary with a KlHttpServer/KlHttpClient: the framework
 * auto-wires each session's provider from `KlHttpServerConfig.sockets`/`KlHttpClientConfig.sockets`
 * via the `KlTls.set_socket_provider` vtable hook (which overrides this ctx default
 * at handshake time). Use this setter for **standalone** KlTls use — driving the
 * vtable directly without connection.c/client.c. Set once on the context, before
 * any handshake; every KlTls the factory creates inherits it. Applies only to the
 * synchronous socket-BIO path — the completion (memory-BIO) mode ignores it.
 *
 * @param ctx Server or client context.
 * @param sp  Socket provider to route through, or NULL for the host default.
 * @return 0 on success, -1 on error (NULL ctx).
 */
struct KlSocketProvider;
int kl_tls_mbedtls_ctx_set_socket_provider(KlTlsCtx *ctx,
                                           const struct KlSocketProvider *sp);

#endif /* KEEL_TLS_MBEDTLS_H */
