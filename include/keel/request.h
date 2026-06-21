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

typedef struct {
    const char *name;  size_t name_len;
    const char *value; size_t value_len;
} KlHeader;

/**
 * @brief Immutable parsed-request snapshot.
 *
 * Under KEEL_SEAL_REQUEST=1, an instance of this struct is allocated
 * in a per-connection sealed arena between routing and the first
 * user-code dispatch (pre-body middleware).  The bytes the pointers
 * reference (method/path/query/headers/params) are copies into the
 * same arena.  After seal, the entire region is mprotect(PROT_READ);
 * any write through these pointers SIGSEGVs.
 *
 * Without KEEL_SEAL_REQUEST, this type is still defined but no
 * sealed-arena copy is made; the legacy `KlRequest::method` /
 * `headers[]` / etc. fields remain the source of truth and the
 * accessors below resolve to those directly.
 *
 * Do NOT access `sealed->method` etc. directly from handler code.
 * Use the `kl_request_*` accessor inlines below; they hide the
 * dual-mode lookup and stay source-compatible across both builds.
 */
typedef struct KlSealedRequest {
    const char *method;       size_t method_len;
    const char *path;         size_t path_len;
    const char *query;        size_t query_len;
    int        version_major;
    int        version_minor;

    KlHeader   headers[KL_MAX_HEADERS];
    int        num_headers;

    size_t     content_length;
    int        chunked;

    KlParam    params[KL_MAX_PARAMS];
    int        num_params;
} KlSealedRequest;

struct KlRequest {
    /* ─── Mutable scratch (always heap, never sealed) ─────────────── */
    int keep_alive;              /**< Connection keep-alive (parser-set, framework may force to 0 on error). */
    KlBodyReader *body_reader;   /**< Set by connection layer after seal, NULL if no body. */
    void *ctx;                   /**< Opaque per-request context, settable by middleware/handlers. */
    void *_server_ctx;           /**< Opaque — set to KlConn* by connection layer (do not modify). */

#ifdef KEEL_SEAL_REQUEST
    /* ─── Sealed snapshot (mprotect-RO between seal point and request end) */
    const KlSealedRequest *sealed; /**< Set by snapshot_and_seal; NULL pre-seal. */
#endif

    /* ─── Parser scratch (zero-copy ptrs into read_buf during parse) ─
     *
     * Under KEEL_SEAL_REQUEST=1 these are STILL populated by the
     * parser (the snapshot reads them to build the sealed copy) but
     * handler code MUST NOT read them directly — use the accessors
     * below.  Without KEEL_SEAL_REQUEST, accessors resolve here.
     */
    const char *method;          size_t method_len;
    const char *path;            size_t path_len;
    const char *query;           size_t query_len;
    int version_major;
    int version_minor;

    KlHeader headers[KL_MAX_HEADERS];
    int num_headers;

    size_t content_length;
    int chunked;

    KlParam params[KL_MAX_PARAMS];
    int num_params;
};

/** @brief Forward declaration — full definition in connection.h. */
typedef struct KlConn KlConn;

/** @brief Typed accessor for the connection handle (preferred over raw _server_ctx). */
static inline KlConn *kl_request_conn(const KlRequest *req) {
    return (KlConn *)req->_server_ctx;
}

/* ─── Field accessors (sealed-aware) ──────────────────────────────────
 *
 * Under KEEL_SEAL_REQUEST=1 each accessor reads through `req->sealed->*`
 * (which is mprotect-RO between snapshot and request end).  Without
 * KEEL_SEAL_REQUEST, accessors read the legacy direct fields and behave
 * identically.  Handler/middleware code MUST go through the accessors;
 * direct field access works in the legacy build but breaks under seal
 * mode (the mutable fields stay live for parser use but are NOT what
 * the handler should read).
 *
 * The macro hides the dual lookup so accessor implementations stay
 * one-line wrappers.  Boot-cost is zero in either mode (compile-time
 * branch).
 */
#ifdef KEEL_SEAL_REQUEST
/* When KEEL_SEAL_REQUEST is on, accessors prefer the sealed snapshot
 * (mprotect-RO between snapshot and request end).  They fall back to
 * the legacy direct fields when `sealed` is NULL: production code
 * paths always have it set by snapshot_and_seal in the connection
 * layer, but test scaffolding that constructs a KlRequest directly
 * (without a connection) leaves sealed=NULL, and we want those tests
 * to keep compiling and working without seal-specific fixtures.
 *
 * The fallback does not meaningfully weaken defence: `sealed` itself
 * is a pointer in the mutable scratch half of KlRequest, so an
 * attacker who can overwrite parsed bytes can equally overwrite
 * `sealed`.  The seal raises the cost of corrupting the bytes
 * themselves; it can't defend its own bypass without sealing the
 * containing struct (which is infeasible for KlConn-embedded
 * KlRequest — see docs/security.md § 4e for the threat model). */
#  define KL_REQ_FIELD(req, field) ((req)->sealed ? (req)->sealed->field : (req)->field)
#  define KL_REQ_HEADERS(req)      ((req)->sealed ? (req)->sealed->headers : (req)->headers)
#  define KL_REQ_NUM_HEADERS(req)  ((req)->sealed ? (req)->sealed->num_headers : (req)->num_headers)
#  define KL_REQ_PARAMS(req)       ((req)->sealed ? (req)->sealed->params : (req)->params)
#  define KL_REQ_NUM_PARAMS(req)   ((req)->sealed ? (req)->sealed->num_params : (req)->num_params)
#else
#  define KL_REQ_FIELD(req, field) ((req)->field)
#  define KL_REQ_HEADERS(req)      ((req)->headers)
#  define KL_REQ_NUM_HEADERS(req)  ((req)->num_headers)
#  define KL_REQ_PARAMS(req)       ((req)->params)
#  define KL_REQ_NUM_PARAMS(req)   ((req)->num_params)
#endif

/** @brief HTTP method (e.g. "GET").  Not NUL-terminated; pair with `_len`. */
static inline const char *kl_request_method(const KlRequest *req) {
    return KL_REQ_FIELD(req, method);
}
static inline size_t kl_request_method_len(const KlRequest *req) {
    return KL_REQ_FIELD(req, method_len);
}

/** @brief Request path (URI before '?').  Not NUL-terminated; pair with `_len`. */
static inline const char *kl_request_path(const KlRequest *req) {
    return KL_REQ_FIELD(req, path);
}
static inline size_t kl_request_path_len(const KlRequest *req) {
    return KL_REQ_FIELD(req, path_len);
}

/** @brief Query string (after '?'), or NULL.  Not NUL-terminated; pair with `_len`. */
static inline const char *kl_request_query(const KlRequest *req) {
    return KL_REQ_FIELD(req, query);
}
static inline size_t kl_request_query_len(const KlRequest *req) {
    return KL_REQ_FIELD(req, query_len);
}

static inline int kl_request_version_major(const KlRequest *req) {
    return KL_REQ_FIELD(req, version_major);
}
static inline int kl_request_version_minor(const KlRequest *req) {
    return KL_REQ_FIELD(req, version_minor);
}

static inline size_t kl_request_content_length(const KlRequest *req) {
    return KL_REQ_FIELD(req, content_length);
}
static inline int kl_request_is_chunked(const KlRequest *req) {
    return KL_REQ_FIELD(req, chunked);
}

static inline int kl_request_num_headers(const KlRequest *req) {
    return KL_REQ_NUM_HEADERS(req);
}
static inline int kl_request_num_params(const KlRequest *req) {
    return KL_REQ_NUM_PARAMS(req);
}

/** @brief Get a header by index (0 .. num_headers-1).
 *  @return KlHeader (the actual pointers are not NUL-terminated). */
static inline KlHeader kl_request_header_at(const KlRequest *req, int idx) {
    if (idx < 0 || idx >= KL_REQ_NUM_HEADERS(req)) {
        KlHeader empty = {0};
        return empty;
    }
    return KL_REQ_HEADERS(req)[idx];
}

/** @brief Get a route param by index (0 .. num_params-1). */
static inline KlParam kl_request_param_at(const KlRequest *req, int idx) {
    if (idx < 0 || idx >= KL_REQ_NUM_PARAMS(req)) {
        KlParam empty = {0};
        return empty;
    }
    return KL_REQ_PARAMS(req)[idx];
}

/* ─── Mutable scratch accessors ───────────────────────────────────────
 *
 * These always read/write the heap-mutable scratch fields on KlRequest
 * itself — never the sealed snapshot.  Use these for the documented
 * mutable fields (ctx, body_reader, keep_alive).
 */

static inline void *kl_request_ctx(const KlRequest *req) {
    return req->ctx;
}
static inline void kl_request_set_ctx(KlRequest *req, void *ctx) {
    req->ctx = ctx;
}
static inline int kl_request_keep_alive(const KlRequest *req) {
    return req->keep_alive;
}
static inline KlBodyReader *kl_request_body_reader(const KlRequest *req) {
    return req->body_reader;
}

/* ─── Header / param lookup helpers ───────────────────────────────────
 *
 * These iterate the headers/params arrays — under KEEL_SEAL_REQUEST=1
 * they read from `req->sealed->headers` / `sealed->params`, otherwise
 * from `req->headers` / `req->params`.  The macros KL_REQ_HEADERS /
 * KL_REQ_NUM_HEADERS / KL_REQ_PARAMS / KL_REQ_NUM_PARAMS hide the
 * dispatch so the loop bodies stay identical.
 */

/** @brief Find header by name (case-insensitive).
 *  @return Null-terminated value pointer, or NULL if not found.
 *  @note Header values from the parser are NOT NUL-terminated; this
 *        return is for convenience when you know the upstream wrote a
 *        terminator (rare).  Prefer `kl_request_header_len`. */
static inline const char *kl_request_header(const KlRequest *req,
                                            const char *name) {
    size_t nlen = strlen(name);
    const KlHeader *h = KL_REQ_HEADERS(req);
    int n = KL_REQ_NUM_HEADERS(req);
    for (int i = 0; i < n; i++) {
        if (h[i].name_len == nlen &&
            strncasecmp(h[i].name, name, nlen) == 0) {
            return h[i].value;
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
    const KlHeader *h = KL_REQ_HEADERS(req);
    int n = KL_REQ_NUM_HEADERS(req);
    for (int i = 0; i < n; i++) {
        if (h[i].name_len == nlen &&
            strncasecmp(h[i].name, name, nlen) == 0) {
            if (out_len) *out_len = h[i].value_len;
            return h[i].value;
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
    const KlParam *p = KL_REQ_PARAMS(req);
    int n = KL_REQ_NUM_PARAMS(req);
    for (int i = 0; i < n; i++) {
        if (p[i].name_len == nlen &&
            memcmp(p[i].name, name, nlen) == 0) {
            if (out_len) *out_len = p[i].value_len;
            return p[i].value;
        }
    }
    return NULL;
}

#endif
