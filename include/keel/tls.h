#ifndef KEEL_TLS_H
#define KEEL_TLS_H

#include <keel/allocator.h>
#include <keel/handle.h>   /* KlSocketHandle, kl_ssize_t */
#include <stddef.h>
#include <stdint.h>

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

/* A KlSocketProvider (keel/socket.h) — forward-declared so the optional
 * set_socket_provider hook can route the transport's socket I/O without this
 * public header depending on the socket seam. */
struct KlSocketProvider;

/**
 * @brief Verified peer (client) certificate identity from an mTLS handshake.
 *
 * Filled by the optional KlTls::peer_cert method. `verified` reports whether
 * the certificate passed CA-chain validation — a certificate can be *present*
 * (client sent one) yet *unverified* (chain/expiry/hostname failure) when the
 * server is configured for optional client auth. Always check `verified` before
 * trusting the identity fields.
 *
 * String fields are always NUL-terminated (empty string when the field is
 * absent from the certificate). `der` points into backend-owned memory that
 * remains valid only until the next reset()/destroy() on the session — copy it
 * if you need it beyond the current request.
 */
typedef struct {
    int      verified;               /**< 1 = passed CA verification, 0 = present but not verified */
    char     subject_cn[256];        /**< Subject CommonName, or "" */
    char     san[512];               /**< Comma-separated DNS/IP SANs, or "" */
    char     issuer_cn[256];         /**< Issuer CommonName, or "" */
    char     fingerprint_sha256[65]; /**< Lowercase hex SHA-256 of the DER cert, or "" */
    int64_t  not_before;             /**< Validity start (unix time), or 0 */
    int64_t  not_after;              /**< Validity end (unix time), or 0 */
    const uint8_t *der;              /**< Raw DER certificate (backend-owned) */
    size_t   der_len;                /**< Length of `der`, 0 if none */
} KlPeerCert;

struct KlTls {
    /**
     * @brief Non-blocking handshake step. Call repeatedly until OK or ERROR.
     * @param self TLS session.
     * @param fd   Socket file descriptor.
     * @return KL_TLS_OK on completion, WANT_READ/WANT_WRITE to retry, ERROR on failure.
     */
    KlTlsResult (*handshake)(KlTls *self, KlSocketHandle fd);

    /**
     * @brief Decrypt: read up to len bytes of plaintext into buf.
     * @return Bytes read (>0), 0 for WANT_READ, -1 for error.
     */
    kl_ssize_t (*read)(KlTls *self, KlSocketHandle fd, void *buf, size_t len);

    /**
     * @brief Encrypt: write up to len bytes of plaintext from buf.
     * @return Bytes written (>0), 0 for WANT_WRITE, -1 for error.
     */
    kl_ssize_t (*write)(KlTls *self, KlSocketHandle fd, const void *buf, size_t len);

    /**
     * @brief Initiate TLS shutdown (close_notify).
     * @return KlTlsResult indicating completion or need for more I/O.
     */
    KlTlsResult (*shutdown)(KlTls *self, KlSocketHandle fd);

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

    /**
     * @brief Extract the verified peer (client) certificate identity.
     *
     * Server-side mTLS: fills `*out` with the client certificate presented
     * during the handshake. Optional — set to NULL if the backend does not
     * support client-certificate extraction.
     * @param self TLS session (handshake must be complete).
     * @param out  Caller-provided struct to populate.
     * @return 0 if a certificate was present and `*out` filled, -1 if none.
     */
    int (*peer_cert)(KlTls *self, KlPeerCert *out);

    /**
     * @brief Completion-mode transport (optional — NULL on backends that only do
     * the synchronous socket BIO). When both feed_input and drain_output are set,
     * the caller drives the transport: it feeds received ciphertext and drains the
     * engine's outgoing ciphertext, and handshake()/read()/write() then operate on
     * those buffers instead of the socket fd (their fd argument is ignored). Used by
     * the completion event loop (IOCP); a readiness loop never calls these, so a
     * backend that leaves them NULL — and every readiness path — is unaffected.
     *
     * feed_input: hand `len` bytes of received ciphertext to the engine's input.
     *   Returns 0 on success, -1 on error (e.g. input buffer full).
     */
    int (*feed_input)(KlTls *self, const void *cipher, size_t len);

    /**
     * @brief Drain up to `cap` bytes of the engine's pending outgoing ciphertext
     * into `buf` (what handshake()/write() produced). Returns bytes written (>= 0),
     * or -1 on error. Optional — see feed_input.
     */
    kl_ssize_t (*drain_output)(KlTls *self, void *buf, size_t cap);

    /**
     * @brief Route the transport's socket I/O through a KlSocketProvider (optional).
     *
     * On the synchronous socket-BIO path (readiness loops), the backend does its
     * ciphertext send/recv on the socket `fd`. By default that uses the built-in
     * host socket ops; when this hook is set, KEEL's server/client call it before
     * the handshake with the *connection's own* provider (`KlHttpServerConfig.sockets` /
     * `KlHttpClientConfig.sockets`), so TLS I/O automatically matches the connection's
     * socket provider — e.g. a non-kernel stack (lwIP) whose descriptors are not
     * host fds. Passing NULL selects the host default.
     *
     * Optional — set to NULL if the backend only ever runs on host sockets. It has
     * no effect on the completion (memory-BIO feed_input/drain_output) path, which
     * never touches the socket fd. Idempotent; safe to call on every handshake.
     */
    void (*set_socket_provider)(KlTls *self, const struct KlSocketProvider *sp);

    /**
     * @brief Did the peer cleanly close the TLS session? (optional)
     *
     * The read() contract has no distinct EOF code — a clean close and a fatal
     * error both surface as -1 (0 means WANT_READ). That is fine for a length-
     * delimited body, but a *close-delimited* response (HTTP/1.0, or `Connection:
     * close` with no `Content-Length`) is finalized precisely by the peer closing,
     * so the reader must tell a clean shutdown apart from an error.
     *
     * After read() returns -1, a caller may consult at_eof(): it returns 1 iff that
     * -1 was a clean TLS shutdown (peer sent close_notify / graceful transport EOF),
     * 0 otherwise. Lets the client treat a clean TLS EOF like a socket recv() of 0
     * (finalize an in-flight response) instead of reporting KL_ERR_IO.
     *
     * Optional — NULL if the backend cannot report it; callers must treat a NULL
     * hook as "unknown", preserving the pre-existing error behavior.
     * @return 1 if the peer cleanly closed the session, else 0.
     */
    int (*at_eof)(KlTls *self);
};

/**
 * @brief True iff every REQUIRED KlTls callback is present.
 *
 * The 7 required ops (handshake/read/write/shutdown/pending/reset/destroy) must all be
 * set; alpn_protocol/peer_cert/feed_input/drain_output/at_eof are optional. A factory that
 * returns a session missing any required op — including `destroy` — is unusable, and a
 * later cleanup path that blindly called the missing op would crash. Callers (e.g.
 * kl_http_server_init) reject such a session with KL_ERR_TLS_VTABLE and free it via its own
 * `destroy` only when that pointer is non-NULL. Header-only so hosted + freestanding share
 * one definition.
 */
static inline int kl_tls_vtable_valid(const KlTls *t) {
    return t && t->handshake && t->read && t->write &&
           t->shutdown && t->pending && t->reset && t->destroy;
}

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
 * @brief TLS configuration for KlHttpServerConfig.
 */
typedef struct {
    KlTlsCtx    *ctx;          /**< Shared context (certs/keys) — user-owned */
    KlTlsFactory factory;      /**< Creates per-connection KlTls */
    void (*ctx_destroy)(KlTlsCtx *ctx);  /**< Optional cleanup at server shutdown */
} KlTlsConfig;

#endif
