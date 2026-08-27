/*
 * http_router_internal.h: INTERNAL. The concrete KlHttpMiddlewareEntry layout.
 *
 * KlHttpMiddlewareEntry is opaque on the public surface (forward-declared in <keel/http_router.h>;
 * KlHttpRouter holds it by pointer). Only the router implementation (src/protocols/http/http_router.c)
 * needs the layout; include this ONLY from there and from explicitly-justified white-box tests.
 * KlHttpRoute stays public and concrete (kl_http_router_match returns it), so it is NOT here.
 */
#ifndef KEEL_SRC_HTTP_ROUTER_INTERNAL_H
#define KEEL_SRC_HTTP_ROUTER_INTERNAL_H

#include <keel/http_router.h>   /* KlHttpMiddlewareEntry forward decl + KlHttpMiddleware + KlHttpRouter */
#include <stddef.h>

struct KlHttpMiddlewareEntry {
    const char *method;    /* HTTP method filter */
    const char *pattern;   /* URL pattern filter */
    size_t method_len;     /* Length of method string */
    size_t pattern_len;    /* Length of pattern string */
    KlHttpMiddleware fn;    /* Middleware function */
    void *user_data;       /* Opaque data passed to fn */
};

#endif /* KEEL_SRC_HTTP_ROUTER_INTERNAL_H */
