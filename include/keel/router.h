#ifndef KEEL_ROUTER_H
#define KEEL_ROUTER_H

#include <keel/allocator.h>
#include <keel/request.h>
#include <keel/response.h>
#include <keel/body_reader.h>
#include <stddef.h>

/** @brief Route handler function. */
typedef void (*KlHandler)(KlRequest *req, KlResponse *res, void *user_data);

/**
 * @brief Middleware function signature.
 * @return 0 to continue to next middleware/handler, non-zero to short-circuit
 *         (response must already be written by the middleware).
 */
typedef int (*KlMiddleware)(KlRequest *req, KlResponse *res, void *user_data);

typedef struct KlWsServerConfig KlWsServerConfig;

typedef struct {
    const char *method;                /**< HTTP method ("GET", "POST", "*") */
    const char *pattern;               /**< URL pattern ("/path", "/path/:param") */
    size_t method_len;                 /**< Length of method string */
    size_t pattern_len;                /**< Length of pattern string */
    KlHandler handler;                 /**< Handler function */
    void *user_data;                   /**< Opaque data passed to handler */
    KlBodyReaderFactory body_reader;   /**< Body reader factory (NULL = discard body) */
    KlWsServerConfig *ws_config;       /**< WebSocket config (non-NULL = WebSocket endpoint) */
} KlRoute;

typedef struct {
    const char *method;    /**< HTTP method filter */
    const char *pattern;   /**< URL pattern filter */
    size_t method_len;     /**< Length of method string */
    size_t pattern_len;    /**< Length of pattern string */
    KlMiddleware fn;       /**< Middleware function */
    void *user_data;       /**< Opaque data passed to fn */
} KlMiddlewareEntry;

typedef struct KlRouter {
    KlRoute *routes;                   /**< Route table array */
    int count;                         /**< Number of registered routes */
    int capacity;                      /**< Route table capacity */

    KlMiddlewareEntry *middleware;     /**< Pre-body middleware array */
    int mw_count;                      /**< Number of pre-body middleware entries */
    int mw_capacity;                   /**< Pre-body middleware capacity */

    KlMiddlewareEntry *post_middleware; /**< Post-body middleware array */
    int post_mw_count;                 /**< Number of post-body middleware entries */
    int post_mw_capacity;              /**< Post-body middleware capacity */

    KlAllocator *alloc;                /**< Allocator for table growth */
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
 * @param r           Router instance.
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
 * @param r          Router instance.
 * @param method     Request method string.
 * @param method_len Length of method string.
 * @param path       Request path string.
 * @param path_len   Length of path string.
 * @param matched    Receives the matched route, or NULL.
 * @param params     Receives extracted :param values.
 * @param num_params Receives the number of extracted params.
 * @return 200 on match, 404 if no path matches, 405 if path matches but method doesn't.
 */
int  kl_router_match(KlRouter *r, const char *method, size_t method_len,
                     const char *path, size_t path_len,
                     KlRoute **matched, KlParam *params, int *num_params);

/**
 * @brief Register pre-body middleware that runs before body reading.
 * @param r       Router instance.
 * @param method  HTTP method filter ("GET", "POST", "*" for any).
 * @param pattern URL pattern — exact match or prefix with trailing slash-star.
 * @param fn      Middleware function. Return 0 to continue, non-zero to short-circuit.
 * @param user_data Passed to fn on each invocation.
 * @return 0 on success, -1 on allocation failure.
 */
int  kl_router_use(KlRouter *r, const char *method, const char *pattern,
                   KlMiddleware fn, void *user_data);

/**
 * @brief Register post-body middleware that runs after body reading.
 *
 * Post-body middleware can access req->body_reader data. Short-circuiting
 * preserves keep_alive since the body has already been consumed.
 *
 * @param r       Router instance.
 * @param method  HTTP method filter ("GET", "POST", "*" for any).
 * @param pattern URL pattern — exact match or prefix with trailing slash-star.
 * @param fn      Middleware function. Return 0 to continue, non-zero to short-circuit.
 * @param user_data Passed to fn on each invocation.
 * @return 0 on success, -1 on allocation failure.
 */
int  kl_router_use_post(KlRouter *r, const char *method, const char *pattern,
                        KlMiddleware fn, void *user_data);

/**
 * @brief Run all matching pre-body middleware in registration order.
 * @return 0 if all passed, non-zero if a middleware short-circuited.
 */
int  kl_router_run_middleware(KlRouter *r, KlRequest *req, KlResponse *res);

/**
 * @brief Run all matching post-body middleware in registration order.
 * @return 0 if all passed, non-zero if a middleware short-circuited.
 */
int  kl_router_run_post_middleware(KlRouter *r, KlRequest *req, KlResponse *res);

/**
 * @brief Run a fully-formed synthetic request through the router pipeline:
 *        match → pre-body middleware → post-body middleware → handler.
 *
 * The caller is responsible for initialising `req` (method, path,
 * headers, optional `body_reader`) and `res` (via `kl_response_init`).
 * On return, `res` holds the response state the handler (or a
 * short-circuiting middleware) produced; the caller is responsible
 * for `kl_response_free` and for inspecting `res->status`,
 * `res->body`, `res->hdr_buf`, etc.
 *
 * `req->num_params` / `req->params` are filled in from the match
 * before the pipeline runs; callers do not need to pre-populate them.
 *
 * If `run_middleware` is 0, pre- and post-body middleware are skipped
 * (only the matched handler runs). When the match fails (404/405) and
 * middleware is disabled, no handler is invoked and the caller should
 * read the return value to know what happened.
 *
 * This is the in-process counterpart to the network-driven dispatch in
 * `connection.c` / `h2.c`. Hull's test harness uses it; user code can
 * use it for synthetic requests (e.g. agent-API self-calls).
 *
 * @param r              Router instance.
 * @param req            Pre-built request (must outlive the call).
 * @param res            Pre-initialised response (the call writes into it).
 * @param run_middleware Non-zero to run pre- and post-body middleware.
 * @return 200 if the handler ran (or middleware short-circuited with a
 *         success-shaped response); 404 if no path matched; 405 if a
 *         path matched but the method didn't; non-zero short-circuit
 *         code if middleware short-circuited; -1 on invalid arguments.
 */
int  kl_router_dispatch_synthetic(KlRouter *r, KlRequest *req,
                                   KlResponse *res, int run_middleware);

/** @brief Free router resources. */
void kl_router_free(KlRouter *r);

#endif
