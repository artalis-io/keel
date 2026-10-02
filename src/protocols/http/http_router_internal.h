/*
 * http_router_internal.h: INTERNAL. The concrete KlHttpMiddlewareEntry layout.
 *
 * KlHttpMiddlewareEntry is opaque on the public surface (forward-declared in <keel/http_router.h>;
 * KlHttpRouter holds it by pointer). Only the router implementation (src/protocols/http/http_router.c)
 * needs the layout; include this ONLY from there, from the HTTP/1 connection (the post-body split
 * below), and from explicitly-justified white-box tests.
 * KlHttpRoute stays public and concrete (kl_http_router_match returns it), so it is NOT here.
 *
 * Also the connection-side split of post-body middleware matching (http_connection.c): an HTTP/1
 * body read reuses read_buf from offset 0, overwriting the request line that req->method and
 * req->path point into, so the connection matches post-body middleware at header time and runs
 * the recorded set once the body is in.
 */
#ifndef KEEL_SRC_HTTP_ROUTER_INTERNAL_H
#define KEEL_SRC_HTTP_ROUTER_INTERNAL_H

#include <keel/http_router.h>   /* KlHttpMiddlewareEntry forward decl + KlHttpMiddleware + KlHttpRouter */
#include <stddef.h>
#include <stdint.h>

struct KlHttpMiddlewareEntry {
    const char *method;    /* HTTP method filter */
    const char *pattern;   /* URL pattern filter */
    size_t method_len;     /* Length of method string */
    size_t pattern_len;    /* Length of pattern string */
    KlHttpMiddleware fn;    /* Middleware function */
    void *user_data;       /* Opaque data passed to fn */
};

/* Bit i set = post-body middleware i matches req's method and path. Call while those are valid. */
uint64_t kl_http_router_post_match(const KlHttpRouter *r, const KlHttpRequest *req);

/* Run the post-body middleware recorded in `matched` (from kl_http_router_post_match), in
 * registration order, without looking at req->method or req->path again. */
int kl_http_router_run_post_matched(KlHttpRouter *r, KlHttpRequest *req, KlHttpResponse *res,
                                    uint64_t matched);

#endif /* KEEL_SRC_HTTP_ROUTER_INTERNAL_H */
