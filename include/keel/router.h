#ifndef KEEL_ROUTER_H
#define KEEL_ROUTER_H

#include <keel/allocator.h>
#include <keel/request.h>
#include <keel/response.h>
#include <keel/body_reader.h>
#include <stddef.h>

typedef void (*KlHandler)(KlRequest *req, KlResponse *res, void *user_data);

/**
 * @brief Middleware function signature.
 * @return 0 to continue to next middleware/handler, non-zero to short-circuit
 *         (response must already be written by the middleware).
 */
typedef int (*KlMiddleware)(KlRequest *req, KlResponse *res, void *user_data);

typedef struct KlWsConfig KlWsConfig;

typedef struct {
    const char *method;
    const char *pattern;
    KlHandler handler;
    void *user_data;
    KlBodyReaderFactory body_reader;   /* NULL = discard body */
    KlWsConfig *ws_config;             /* non-NULL = WebSocket endpoint */
} KlRoute;

typedef struct {
    const char *method;
    const char *pattern;
    KlMiddleware fn;
    void *user_data;
} KlMiddlewareEntry;

typedef struct KlRouter {
    KlRoute *routes;
    int count;
    int capacity;

    KlMiddlewareEntry *middleware;
    int mw_count;
    int mw_capacity;

    KlAllocator *alloc;
} KlRouter;

/**
 * @brief Initialize a router with an empty route table.
 * @param r     Router to initialize.
 * @param alloc Allocator for route table growth.
 * @return 0 on success, -1 on allocation failure.
 */
int  kl_router_init(KlRouter *r, KlAllocator *alloc);

/**
 * @brief Register a route. Pattern supports :param segments (e.g. "/users/:id").
 * @param method      HTTP method ("GET", "POST", "*" for any).
 * @param pattern     URL pattern to match.
 * @param handler     Handler function invoked on match.
 * @param user_data   Passed to handler and body reader factory.
 * @param body_reader Factory for body reader, or NULL to discard body.
 * @return 0 on success, -1 on allocation failure.
 */
int  kl_router_add(KlRouter *r, const char *method, const char *pattern,
                   KlHandler handler, void *user_data,
                   KlBodyReaderFactory body_reader);

/**
 * @brief Match a request against registered routes.
 *        HEAD requests automatically fall back to GET routes.
 * @param matched    Receives the matched route, or NULL.
 * @param params     Receives extracted :param values.
 * @param num_params Receives the number of extracted params.
 * @return 200 on match, 404 if no path matches, 405 if path matches but method doesn't.
 */
int  kl_router_match(KlRouter *r, const char *method, size_t method_len,
                     const char *path, size_t path_len,
                     KlRoute **matched, KlParam *params, int *num_params);

/**
 * @brief Register middleware that runs before matched handlers.
 * @param method  HTTP method filter ("GET", "POST", "*" for any).
 * @param pattern URL pattern — exact match or prefix with trailing slash-star.
 * @param fn      Middleware function. Return 0 to continue, non-zero to short-circuit.
 * @param user_data Passed to fn on each invocation.
 * @return 0 on success, -1 on allocation failure.
 */
int  kl_router_use(KlRouter *r, const char *method, const char *pattern,
                   KlMiddleware fn, void *user_data);

/**
 * @brief Run all matching middleware in registration order.
 * @return 0 if all passed, non-zero if a middleware short-circuited.
 */
int  kl_router_run_middleware(KlRouter *r, KlRequest *req, KlResponse *res);

/** @brief Free router resources. */
void kl_router_free(KlRouter *r);

#endif
