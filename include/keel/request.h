#ifndef KEEL_REQUEST_H
#define KEEL_REQUEST_H

#include <stddef.h>
#include <string.h>
#include <strings.h>

/** @brief Maximum number of request headers. */
#define KL_MAX_HEADERS 64

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

typedef struct KlRequest KlRequest;

struct KlRequest {
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

    void *_server_ctx;           /**< Opaque — set to KlConn* by connection layer (do not modify). */
};

/** @brief Forward declaration — full definition in connection.h. */
typedef struct KlConn KlConn;

/** @brief Typed accessor for the connection handle (preferred over raw _server_ctx). */
static inline KlConn *kl_request_conn(const KlRequest *req) {
    return (KlConn *)req->_server_ctx;
}

/**
 * @brief Read-side body flow control — pause / resume request-body reading.
 *
 * A streamed-body consumer whose downstream sink is full calls kl_request_pause_body() to stop
 * Keel reading more body bytes off the connection, bounding accumulation without aborting the
 * body. It re-enables reading with kl_request_resume_body() once the sink drains. Both are
 * idempotent and must be called on the event-loop thread (e.g. from the body reader's on_data,
 * or later from a watcher/timer/thread-pool completion that drives the sink).
 *
 * Semantics:
 *  - Readiness: pause drops READ interest immediately; resume re-arms it.
 *  - Completion: pause stops posting the next recv; the one already-submitted recv may still
 *    deliver a final chunk (bounded to ≤1 in flight); resume posts a fresh recv.
 *  - A conn that stays paused is NOT exempt from the idle-read timeout — an indefinitely paused
 *    consumer is eventually timed out (slowloris/backpressure defense).
 *  - Pausing before/outside KL_CONN_READING_BODY is a no-op-safe state set; resume only re-arms
 *    when a pause is in effect.
 */
/* const: they mutate the *connection's* read state (via kl_request_conn), not the KlRequest. */
void kl_request_pause_body(const KlRequest *req);
void kl_request_resume_body(const KlRequest *req);

/** @brief Find header by name (case-insensitive).
 *  @return Null-terminated value pointer, or NULL if not found. */
static inline const char *kl_request_header(const KlRequest *req,
                                            const char *name) {
    size_t nlen = strlen(name);
    for (int i = 0; i < req->num_headers; i++) {
        if (req->headers[i].name_len == nlen &&
            strncasecmp(req->headers[i].name, name, nlen) == 0) {
            return req->headers[i].value;
        }
    }
    return NULL;
}

/** @brief Find header by name (case-insensitive).
 *  @return Value pointer (sets *out_len), or NULL if not found. */
static inline const char *kl_request_header_len(const KlRequest *req,
                                                const char *name,
                                                size_t *out_len) {
    size_t nlen = strlen(name);
    for (int i = 0; i < req->num_headers; i++) {
        if (req->headers[i].name_len == nlen &&
            strncasecmp(req->headers[i].name, name, nlen) == 0) {
            if (out_len) *out_len = req->headers[i].value_len;
            return req->headers[i].value;
        }
    }
    return NULL;
}

/** @brief Find route parameter by name.
 *  @return Value pointer (sets *out_len), or NULL if not found. */
static inline const char *kl_request_param(const KlRequest *req,
                                            const char *name,
                                            size_t *out_len) {
    size_t nlen = strlen(name);
    for (int i = 0; i < req->num_params; i++) {
        if (req->params[i].name_len == nlen &&
            memcmp(req->params[i].name, name, nlen) == 0) {
            if (out_len) *out_len = req->params[i].value_len;
            return req->params[i].value;
        }
    }
    return NULL;
}

#endif
