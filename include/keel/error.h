#ifndef KEEL_ERROR_H
#define KEEL_ERROR_H

/**
 * @brief Diagnostic error codes for Keel public functions.
 *
 * Stored as a field on owning structs (KlHttpServer, KlEventCtx, KlClientResponse,
 * KlClient). Set at the point of return -1, defaults to KL_ERR_NONE (0) via
 * existing memset initialization.
 */
typedef enum {
    KL_ERR_NONE = 0,        /**< No error */

    /* General */
    KL_ERR_INVALID_ARG,     /**< NULL pointer, negative count, bad params */
    KL_ERR_ALLOC,           /**< Allocator returned NULL */
    KL_ERR_OVERFLOW,        /**< Integer overflow guard tripped */

    /* Network */
    KL_ERR_SOCKET,          /**< socket() or fcntl() failed */
    KL_ERR_BIND,            /**< bind() failed (EADDRINUSE, EACCES, etc.) */
    KL_ERR_LISTEN,          /**< listen() failed */
    KL_ERR_CONNECT,         /**< connect() failed or refused */
    KL_ERR_IO,              /**< read/write/send/recv error */
    KL_ERR_TIMEOUT,         /**< Operation timed out */

    /* DNS */
    KL_ERR_DNS,             /**< getaddrinfo / resolver error */

    /* TLS */
    KL_ERR_TLS_INIT,        /**< TLS factory returned NULL */
    KL_ERR_TLS_HANDSHAKE,   /**< TLS handshake failed */
    KL_ERR_TLS_VTABLE,      /**< TLS vtable missing required function */

    /* HTTP */
    KL_ERR_URL,             /**< URL parse error or scheme mismatch */
    KL_ERR_PARSE,           /**< HTTP parse error */
    KL_ERR_TOO_LARGE,       /**< Request/response too large */
    KL_ERR_HEADER,          /**< CRLF injection or header build error */

    /* Event loop */
    KL_ERR_EVENT_INIT,      /**< Event loop init failed */
    KL_ERR_EVENT_ADD,       /**< Event registration failed */

    /* Thread pool */
    KL_ERR_QUEUE_FULL,      /**< Thread pool queue at capacity */
    KL_ERR_THREAD,          /**< pthread_create failed */

    /* IPC */
    KL_ERR_PIPE,            /**< pipe() failed */

    /* Redirect */
    KL_ERR_REDIRECT_LOOP,   /**< Too many redirects */

    /* Compression */
    KL_ERR_COMPRESS,        /**< Compression error */

    /* Proxy */
    KL_ERR_PROXY,           /**< Proxy CONNECT rejected or protocol error */

    /* Capability */
    KL_ERR_UNSUPPORTED,     /**< A requested capability is not supported by the provider (M2) */

    KL_ERR__COUNT           /**< Sentinel — not an error code */
} KlError;

/**
 * @brief Return a short human-readable message for an error code.
 * @param err Error code.
 * @return Static string, never NULL. Returns "unknown error" for out-of-range.
 */
const char *kl_strerror(KlError err);

#endif /* KEEL_ERROR_H */
