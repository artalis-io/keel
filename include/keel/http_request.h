#ifndef KEEL_HTTP_REQUEST_H
#define KEEL_HTTP_REQUEST_H

#include <stddef.h>

/** @brief Maximum number of request headers. */
#define KL_MAX_HEADERS 64

/*
 * request.h is a header-only, zero-allocation part of the public API and is in
 * the freestanding gate (tests/freestanding_headers.c). To stay hosted-libc-free
 * it uses these tiny inline primitives instead of <string.h>/<strings.h>:
 * kl_ascii_strncasecmp for the (ASCII, locale-free) header-name match, and
 * kl_req_strlen/kl_req_memeq for the name-length and byte-compare. Behavior is
 * identical to strlen/memcmp==0; names stay short and match RFC 7230 tokens.
 */

/**
 * @brief ASCII-only, locale-independent case-insensitive compare of the first
 *        @p n bytes of two buffers. Returns 0 when the two @p n-byte spans are
 *        equal ignoring ASCII A-Z/a-z case (header-name matching, RFC 7230).
 */
static inline int kl_ascii_strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return (int)ca - (int)cb;
    }
    return 0;
}

/** @brief Length of a NUL-terminated C string (freestanding strlen). */
static inline size_t kl_req_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/** @brief 1 iff the first @p n bytes of @p a and @p b are equal (freestanding). */
static inline int kl_req_memeq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

/** @brief Forward declaration — full definition in body_reader.h. */
typedef struct KlBodyReader KlBodyReader;

typedef struct {
    const char *name;   /**< Parameter name pointer. */
    size_t name_len;    /**< Parameter name length. */
    const char *value;  /**< Parameter value pointer. */
    size_t value_len;   /**< Parameter value length. */
} KlParam;

/** @brief Maximum number of route parameters. */
#define KL_MAX_PARAMS 16

typedef struct KlHttpRequest KlHttpRequest;

struct KlHttpRequest {
    const char *method;          /**< HTTP method pointer. */
    size_t method_len;           /**< HTTP method length. */
    const char *path;            /**< Request path pointer. */
    size_t path_len;             /**< Request path length. */
    const char *query;           /**< After '?' or NULL. */
    size_t query_len;            /**< Query string length. */
    int version_major;           /**< HTTP major version. */
    int version_minor;           /**< HTTP minor version. */
    int keep_alive;              /**< 1 if connection is keep-alive. */

    struct {
        const char *name;  size_t name_len;
        const char *value; size_t value_len;
    } headers[KL_MAX_HEADERS];  /**< Parsed request headers. */
    int num_headers;             /**< Number of parsed headers. */

    size_t content_length;       /**< Content-Length value. */
    int chunked;                 /**< 1 if Transfer-Encoding: chunked. */
    KlBodyReader *body_reader;   /**< Set by connection layer, NULL if no body. */
    void *ctx;                   /**< Opaque per-request context, set by middleware. */

    KlParam params[KL_MAX_PARAMS]; /**< Matched route parameters. */
    int num_params;              /**< Number of matched route parameters. */

    void *_server_ctx;           /**< Opaque — set to KlHttpConn* by connection layer (do not modify). */
};

/** @brief Forward declaration — full definition in connection.h. */
typedef struct KlHttpConn KlHttpConn;

/** @brief Typed accessor for the connection handle (preferred over raw _server_ctx). */
static inline KlHttpConn *kl_http_request_conn(const KlHttpRequest *req) {
    return (KlHttpConn *)req->_server_ctx;
}

/**
 * @brief Read-side body flow control — pause / resume request-body reading.
 *
 * A streamed-body consumer whose downstream sink is full calls kl_http_request_pause_body() to stop
 * Keel reading more body bytes off the connection, bounding accumulation without aborting the
 * body. It re-enables reading with kl_http_request_resume_body() once the sink drains. Both are
 * idempotent and must be called on the event-loop thread (e.g. from the body reader's on_data,
 * or later from a watcher/timer/thread-pool completion that drives the sink).
 *
 * Semantics:
 *  - Readiness: pause drops READ interest immediately; resume re-arms it.
 *  - Completion: pause stops posting the next recv; the one already-submitted recv may still
 *    deliver a final chunk (bounded to ≤1 in flight); resume posts a fresh recv.
 *  - A conn that stays paused is NOT exempt from the idle-read timeout — an indefinitely paused
 *    consumer is eventually timed out (slowloris/backpressure defense).
 *  - Pausing before/outside KL_HTTP_CONN_READING_BODY is a no-op-safe state set; resume only re-arms
 *    when a pause is in effect.
 */
/* const: they mutate the *connection's* read state (via kl_http_request_conn), not the KlHttpRequest. */
void kl_http_request_pause_body(const KlHttpRequest *req);
void kl_http_request_resume_body(const KlHttpRequest *req);

/** @brief Find header by name (case-insensitive).
 *  @return Null-terminated value pointer, or NULL if not found. */
static inline const char *kl_http_request_header(const KlHttpRequest *req,
                                            const char *name) {
    size_t nlen = kl_req_strlen(name);
    for (int i = 0; i < req->num_headers; i++) {
        if (req->headers[i].name_len == nlen &&
            kl_ascii_strncasecmp(req->headers[i].name, name, nlen) == 0) {
            return req->headers[i].value;
        }
    }
    return NULL;
}

/** @brief Find header by name (case-insensitive).
 *  @return Value pointer (sets *out_len), or NULL if not found. */
static inline const char *kl_http_request_header_len(const KlHttpRequest *req,
                                                const char *name,
                                                size_t *out_len) {
    size_t nlen = kl_req_strlen(name);
    for (int i = 0; i < req->num_headers; i++) {
        if (req->headers[i].name_len == nlen &&
            kl_ascii_strncasecmp(req->headers[i].name, name, nlen) == 0) {
            if (out_len) *out_len = req->headers[i].value_len;
            return req->headers[i].value;
        }
    }
    return NULL;
}

/** @brief Find route parameter by name.
 *  @return Value pointer (sets *out_len), or NULL if not found. */
static inline const char *kl_http_request_param(const KlHttpRequest *req,
                                            const char *name,
                                            size_t *out_len) {
    size_t nlen = kl_req_strlen(name);
    for (int i = 0; i < req->num_params; i++) {
        if (req->params[i].name_len == nlen &&
            kl_req_memeq(req->params[i].name, name, nlen)) {
            if (out_len) *out_len = req->params[i].value_len;
            return req->params[i].value;
        }
    }
    return NULL;
}

#endif
