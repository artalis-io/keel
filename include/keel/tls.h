#ifndef KEEL_TLS_H
#define KEEL_TLS_H

#include <keel/allocator.h>
#include <stddef.h>
#include <sys/types.h>

/**
 * @brief Result codes for non-blocking TLS operations.
 */
typedef enum {
    KL_TLS_OK,          /**< Operation complete */
    KL_TLS_WANT_READ,   /**< Need more data from socket */
    KL_TLS_WANT_WRITE,  /**< Need to flush data to socket */
    KL_TLS_ERROR        /**< Fatal error */
} KlTlsResult;

/**
 * @brief Pluggable TLS session vtable.
 *
 * Users implement this interface to provide TLS (BearSSL, LibreSSL, OpenSSL,
 * rustls-ffi, etc.). One KlTls instance exists per connection slot, pre-allocated
 * at server init and reused via reset() across keep-alive connections.
 */
typedef struct KlTls KlTls;

struct KlTls {
    /**
     * @brief Non-blocking handshake step. Call repeatedly until OK or ERROR.
     * @param self TLS session.
     * @param fd   Socket file descriptor.
     * @return KL_TLS_OK on completion, WANT_READ/WANT_WRITE to retry, ERROR on failure.
     */
    KlTlsResult (*handshake)(KlTls *self, int fd);

    /**
     * @brief Decrypt: read up to len bytes of plaintext into buf.
     * @return Bytes read (>0), 0 for WANT_READ, -1 for error.
     */
    ssize_t (*read)(KlTls *self, int fd, void *buf, size_t len);

    /**
     * @brief Encrypt: write up to len bytes of plaintext from buf.
     * @return Bytes written (>0), 0 for WANT_WRITE, -1 for error.
     */
    ssize_t (*write)(KlTls *self, int fd, const void *buf, size_t len);

    /**
     * @brief Initiate TLS shutdown (close_notify).
     * @return KlTlsResult indicating completion or need for more I/O.
     */
    KlTlsResult (*shutdown)(KlTls *self, int fd);

    /**
     * @brief Bytes buffered inside TLS that can be read without a syscall.
     *
     * Critical for edge-triggered event loops: TLS may decrypt multiple
     * application records in one read. Without pending(), buffered plaintext
     * stalls until the next TCP segment arrives.
     */
    size_t (*pending)(KlTls *self);

    /**
     * @brief Reset for connection reuse (keep-alive). TLS session persists.
     */
    void (*reset)(KlTls *self);

    /**
     * @brief Free all resources.
     */
    void (*destroy)(KlTls *self);

    /**
     * @brief Negotiated ALPN protocol, or NULL.
     * Optional — set to NULL if not supported.
     */
    const char *(*alpn_protocol)(KlTls *self);

    /**
     * @brief Set the expected server hostname for SNI (client mode).
     * Optional — set to NULL if not supported by the backend.
     * Must be called before handshake().
     * @param self     TLS session.
     * @param hostname Server hostname for SNI and certificate verification.
     * @return 0 on success, -1 on error.
     */
    int (*set_hostname)(KlTls *self, const char *hostname);
};

/**
 * @brief Opaque per-server TLS context (certificates, keys, ciphers).
 * User-owned — KEEL never inspects or modifies this.
 */
typedef struct KlTlsCtx KlTlsCtx;

/**
 * @brief Factory creates a per-connection KlTls session from the shared context.
 * Called once per connection slot at server init.
 * @param ctx   Shared TLS context (certs/keys).
 * @param alloc Allocator for session resources.
 * @return New TLS session, or NULL on failure.
 */
typedef KlTls *(*KlTlsFactory)(KlTlsCtx *ctx, KlAllocator *alloc);

/**
 * @brief TLS configuration for KlConfig.
 */
typedef struct {
    KlTlsCtx    *ctx;          /**< Shared context (certs/keys) — user-owned */
    KlTlsFactory factory;      /**< Creates per-connection KlTls */
    void (*ctx_destroy)(KlTlsCtx *ctx);  /**< Optional cleanup at server shutdown */
} KlTlsConfig;

#endif
