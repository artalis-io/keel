#ifndef KEEL_HTTP_ROUTER_H
#define KEEL_HTTP_ROUTER_H

#include <keel/allocator.h>
#include <keel/http_request.h>
#include <keel/http_response.h>
#include <keel/http_body_reader.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif


/** @brief Route handler function. */
typedef void (*KlHttpHandler)(KlHttpRequest *req, KlHttpResponse *res, void *user_data);

/**
 * @brief Middleware function signature.
 * @return 0 to continue to next middleware/handler, non-zero to short-circuit
 *         (response must already be written by the middleware).
 */
typedef int (*KlHttpMiddleware)(KlHttpRequest *req, KlHttpResponse *res, void *user_data);

typedef struct KlWsServerConfig KlWsServerConfig;

typedef struct {
    const char *method;                /**< HTTP method ("GET", "POST", "*") */
    const char *pattern;               /**< URL pattern ("/path", "/path/:param") */
    size_t method_len;                 /**< Length of method string */
    size_t pattern_len;                /**< Length of pattern string */
    KlHttpHandler handler;                 /**< Handler function */
    void *user_data;                   /**< Opaque data passed to handler */
    KlHttpBodyReaderFactory body_reader;   /**< Body reader factory (NULL = discard body) */
    KlWsServerConfig *ws_config;       /**< WebSocket config (non-NULL = WebSocket endpoint) */
    int streaming_handler;             /**< 1 = invoke handler after body_reader setup
                                            BEFORE the body is fully read; the handler
                                            may yield mid-stream and the body reader's
                                            on_data callback is responsible for
                                            resuming. Used by streaming multipart and
                                            similar pull-from-body APIs. */
    int streaming_async;               /**< 1 = invoke handler BEFORE feeding any
                                            leftover body bytes via on_data (v2.2.0+).
                                            Requires the handler to yield on
                                            NEED_DATA; it'll be resumed by the body
                                            reader's on_data callback for the leftover
                                            AND subsequent socket reads.

                                            Implies streaming_handler=1; set both via
                                            kl_http_server_route_streaming_async.

                                            Enables the full error-path mid-stream
                                            early-exit (a structured response can be
                                            written even when the cap fires inside
                                            leftover processing). Routes that opt out
                                            (use the legacy kl_http_server_route_streaming)
                                            still get the partial early-exit covering
                                            caps that fire after dispatch. */
} KlHttpRoute;

/* Registered middleware entry: an opaque, router-internal record. The layout is private to the
 * router implementation (src/protocols/http/http_router_internal.h); KlHttpRouter holds it by
 * pointer, so only this forward declaration is public. */
typedef struct KlHttpMiddlewareEntry KlHttpMiddlewareEntry;

typedef struct KlHttpRouter {
    KlHttpRoute *routes;                   /**< Route table array */
    int count;                         /**< Number of registered routes */
    int capacity;                      /**< Route table capacity */

    KlHttpMiddlewareEntry *middleware;     /**< Pre-body middleware array */
    int mw_count;                      /**< Number of pre-body middleware entries */
    int mw_capacity;                   /**< Pre-body middleware capacity */

    KlHttpMiddlewareEntry *post_middleware; /**< Post-body middleware array */
    int post_mw_count;                 /**< Number of post-body middleware entries */
    int post_mw_capacity;              /**< Post-body middleware capacity */

    KlAllocator *alloc;                /**< Allocator for table growth */
} KlHttpRouter;

/**
 * @brief Initialize a router with an empty route table.
 * @param r     Router to initialize.
 * @param alloc Allocator for route table growth.
 * @return 0 on success, -1 on allocation failure.
 */
int  kl_http_router_init(KlHttpRouter *r, KlAllocator *alloc);

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
int  kl_http_router_add(KlHttpRouter *r, const char *method, const char *pattern,
                   KlHttpHandler handler, void *user_data,
                   KlHttpBodyReaderFactory body_reader);

/**
 * @brief Register a streaming-handler route. Identical to kl_http_router_add
 *        except the handler runs after the body reader is set up but
 *        BEFORE the body is fully received. The handler is expected to
 *        consume the body incrementally (e.g. via the streaming
 *        multipart pull iterator) and may yield mid-stream. The body
 *        reader's on_data callback is responsible for resuming the
 *        yielded handler when more bytes arrive.
 *
 *        Post-body middleware is NOT run for streaming routes (the
 *        handler has already executed by the time the body completes).
 *
 *        A non-NULL body_reader factory is required.
 *
 * @return 0 on success, -1 on allocation failure or NULL body_reader.
 */
int  kl_http_router_add_streaming(KlHttpRouter *r, const char *method, const char *pattern,
                              KlHttpHandler handler, void *user_data,
                              KlHttpBodyReaderFactory body_reader);

/**
 * @brief Register an async-streaming-handler route (v2.2.0+).
 *
 *        Identical to kl_http_router_add_streaming, plus: the handler is
 *        invoked BEFORE any leftover body bytes are fed via on_data.
 *        It MUST yield on NEED_DATA: the body reader's on_data
 *        callback will resume it when bytes arrive (both the leftover
 *        from the headers-read and subsequent socket reads).
 *
 *        Enables the full error-path mid-stream early-exit: if the
 *        body reader rejects bytes (on_data returns -1) at any point,
 *        the parked handler is resumed via on_error and can catch the
 *        parser error to write a structured response. Routes that opt
 *        out (use the legacy kl_http_router_add_streaming) still get the
 *        partial early-exit covering caps that fire after dispatch
 *        but lose the structured response for caps that fire during
 *        leftover processing.
 *
 *        Synchronous C handlers that consume the body without yielding
 *        (i.e. they call into the body reader expecting events to be
 *        immediately available) must use the legacy kl_http_router_add_
 *        streaming; they'll see NEED_DATA on the first call here
 *        and have no way to recover.
 *
 * @param r           Router instance.
 * @param method      HTTP method.
 * @param pattern     URL pattern.
 * @param handler     Yield-on-NEED_DATA handler.
 * @param user_data   Opaque pointer.
 * @param body_reader Body reader factory (NULL rejected).
 * @return 0 on success, -1 on allocation failure or NULL body_reader.
 */
int  kl_http_router_add_streaming_async(KlHttpRouter *r, const char *method,
                                     const char *pattern,
                                     KlHttpHandler handler, void *user_data,
                                     KlHttpBodyReaderFactory body_reader);

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
int  kl_http_router_match(KlHttpRouter *r, const char *method, size_t method_len,
                     const char *path, size_t path_len,
                     KlHttpRoute **matched, KlHttpParam *params, int *num_params);

/**
 * @brief Register pre-body middleware that runs before body reading.
 * @param r       Router instance.
 * @param method  HTTP method filter ("GET", "POST", "*" for any).
 * @param pattern URL pattern: exact match or prefix with trailing slash-star.
 * @param fn      Middleware function. Return 0 to continue, non-zero to short-circuit.
 * @param user_data Passed to fn on each invocation.
 * @return 0 on success, -1 on allocation failure.
 */
int  kl_http_router_use(KlHttpRouter *r, const char *method, const char *pattern,
                   KlHttpMiddleware fn, void *user_data);

/**
 * @brief Register post-body middleware that runs after body reading.
 *
 * Post-body middleware can access req->body_reader data. Short-circuiting
 * preserves keep_alive since the body has already been consumed.
 *
 * @param r       Router instance.
 * @param method  HTTP method filter ("GET", "POST", "*" for any).
 * @param pattern URL pattern: exact match or prefix with trailing slash-star.
 * @param fn      Middleware function. Return 0 to continue, non-zero to short-circuit.
 * @param user_data Passed to fn on each invocation.
 * @return 0 on success, -1 on allocation failure.
 */
int  kl_http_router_use_post(KlHttpRouter *r, const char *method, const char *pattern,
                        KlHttpMiddleware fn, void *user_data);

/**
 * @brief Run all matching pre-body middleware in registration order.
 * @return 0 if all passed, non-zero if a middleware short-circuited.
 */
int  kl_http_router_run_middleware(KlHttpRouter *r, KlHttpRequest *req, KlHttpResponse *res);

/**
 * @brief Run all matching post-body middleware in registration order.
 * @return 0 if all passed, non-zero if a middleware short-circuited.
 */
int  kl_http_router_run_post_middleware(KlHttpRouter *r, KlHttpRequest *req, KlHttpResponse *res);

/**
 * @brief Run a fully-formed synthetic request through the router pipeline:
 *        match → pre-body middleware → post-body middleware → handler.
 *
 * The caller is responsible for initialising `req` (method, path,
 * headers, optional `body_reader`) and `res` (via `kl_http_response_init`).
 * On return, `res` holds the response state the handler (or a
 * short-circuiting middleware) produced; the caller is responsible
 * for `kl_http_response_free` and for inspecting `res->status`,
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
 * This is the in-process counterpart to the network-driven dispatch in the HTTP/1.1 and HTTP/2
 * connection handlers (`src/protocols/http/http_connection.c`, `src/protocols/http2/`). Hull's test
 * harness uses it; user code can use it for synthetic requests (e.g. agent-API self-calls).
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
int  kl_http_router_dispatch_synthetic(KlHttpRouter *r, KlHttpRequest *req,
                                   KlHttpResponse *res, int run_middleware);

/** @brief Free router resources. */
void kl_http_router_free(KlHttpRouter *r);

#ifdef __cplusplus
}
#endif

#endif
