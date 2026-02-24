#ifndef KEEL_ROUTER_H
#define KEEL_ROUTER_H

#include <keel/allocator.h>
#include <keel/request.h>
#include <keel/response.h>
#include <keel/body_reader.h>
#include <stddef.h>

typedef void (*KlHandler)(KlRequest *req, KlResponse *res, void *user_data);

typedef struct {
    const char *name;   size_t name_len;
    const char *value;  size_t value_len;
} KlParam;

#define KL_MAX_PARAMS 16

typedef struct {
    const char *method;
    const char *pattern;
    KlHandler handler;
    void *user_data;
    KlBodyReaderFactory body_reader;   /* NULL = discard body */
} KlRoute;

typedef struct {
    KlRoute *routes;
    int count;
    int capacity;
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

/** @brief Free router resources. */
void kl_router_free(KlRouter *r);

#endif
