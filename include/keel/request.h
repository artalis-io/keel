#ifndef KEEL_REQUEST_H
#define KEEL_REQUEST_H

#include <stddef.h>
#include <string.h>
#include <strings.h>

#define KL_MAX_HEADERS 64

/* Forward declaration — full definition in body_reader.h */
typedef struct KlBodyReader KlBodyReader;

typedef struct KlRequest KlRequest;

struct KlRequest {
    const char *method;
    size_t method_len;
    const char *path;
    size_t path_len;
    const char *query;      /* after '?' or NULL */
    size_t query_len;
    int version_major;
    int version_minor;
    int keep_alive;

    struct {
        const char *name;  size_t name_len;
        const char *value; size_t value_len;
    } headers[KL_MAX_HEADERS];
    int num_headers;

    size_t content_length;
    int chunked;                 /* 1 if Transfer-Encoding: chunked */
    KlBodyReader *body_reader;   /* set by connection layer, NULL if no body */
    void *ctx;                   /* opaque per-request context, set by middleware */
};

/* Find header by name (case-insensitive). Returns value pointer or NULL.
 * WARNING: returned pointer is NOT null-terminated. Use kl_request_header_len()
 * when you need the value length, or copy with snprintf/memcpy before use. */
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

/* Find header by name (case-insensitive). Returns value pointer and sets *out_len.
 * Returns NULL if not found. */
static inline const char *kl_request_header_len(const KlRequest *req,
                                                const char *name,
                                                size_t *out_len) {
    size_t nlen = strlen(name);
    for (int i = 0; i < req->num_headers; i++) {
        if (req->headers[i].name_len == nlen &&
            strncasecmp(req->headers[i].name, name, nlen) == 0) {
            *out_len = req->headers[i].value_len;
            return req->headers[i].value;
        }
    }
    return NULL;
}

#endif
