#include <keel/router.h>
#include <string.h>
#include <limits.h>

#define KL_ROUTER_INIT_CAP 16

int kl_router_init(KlRouter *r, KlAllocator *alloc) {
    r->alloc = alloc;
    r->count = 0;
    r->capacity = KL_ROUTER_INIT_CAP;
    r->routes = kl_malloc(alloc, sizeof(KlRoute) * (size_t)r->capacity);
    return r->routes ? 0 : -1;
}

int kl_router_add(KlRouter *r, const char *method, const char *pattern,
                  KlHandler handler, void *user_data,
                  KlBodyReaderFactory body_reader) {
    if (r->count >= r->capacity) {
        if (r->capacity > INT_MAX / 2) return -1;
        int new_cap = r->capacity * 2;
        KlRoute *new_routes = kl_realloc(r->alloc, r->routes,
                                         sizeof(KlRoute) * (size_t)r->capacity,
                                         sizeof(KlRoute) * (size_t)new_cap);
        if (!new_routes) return -1;
        r->routes = new_routes;
        r->capacity = new_cap;
    }

    r->routes[r->count] = (KlRoute){
        .method = method,
        .pattern = pattern,
        .handler = handler,
        .user_data = user_data,
        .body_reader = body_reader,
    };
    r->count++;
    return 0;
}

/*
 * Match a single pattern segment against a path segment.
 * Pattern segment starts with ':' for param capture.
 * Returns 1 on match, 0 on mismatch.
 */
static int match_path(const char *pattern, size_t pat_len,
                      const char *path, size_t path_len,
                      KlParam *params, int *num_params) {
    const char *pp = pattern;
    const char *pp_end = pattern + pat_len;
    const char *rp = path;
    const char *rp_end = path + path_len;
    *num_params = 0;

    /* Skip leading slashes */
    if (pp < pp_end && *pp == '/') pp++;
    if (rp < rp_end && *rp == '/') rp++;

    while (pp < pp_end && rp < rp_end) {
        /* Find end of pattern segment */
        const char *ps_end = pp;
        while (ps_end < pp_end && *ps_end != '/') ps_end++;

        /* Find end of path segment */
        const char *rs_end = rp;
        while (rs_end < rp_end && *rs_end != '/') rs_end++;

        size_t ps_len = (size_t)(ps_end - pp);
        size_t rs_len = (size_t)(rs_end - rp);

        if (ps_len > 0 && *pp == ':') {
            /* Param capture — still matches even if params array is full */
            if (*num_params < KL_MAX_PARAMS) {
                params[*num_params] = (KlParam){
                    .name = pp + 1,
                    .name_len = ps_len - 1,
                    .value = rp,
                    .value_len = rs_len,
                };
            }
            (*num_params)++;
        } else {
            /* Literal match */
            if (ps_len != rs_len || memcmp(pp, rp, ps_len) != 0)
                return 0;
        }

        pp = ps_end;
        rp = rs_end;
        if (pp < pp_end && *pp == '/') pp++;
        if (rp < rp_end && *rp == '/') rp++;
    }

    /* Both must be fully consumed */
    return (pp >= pp_end && rp >= rp_end) ? 1 : 0;
}

int kl_router_match(KlRouter *r, const char *method, size_t method_len,
                    const char *path, size_t path_len,
                    KlRoute **matched, KlParam *params, int *num_params) {
    int path_matched = 0;

    for (int i = 0; i < r->count; i++) {
        KlRoute *route = &r->routes[i];
        int np = 0;

        if (!match_path(route->pattern, strlen(route->pattern),
                        path, path_len, params, &np))
            continue;

        path_matched = 1;

        /* Check method */
        if (route->method[0] == '*' ||
            (strlen(route->method) == method_len &&
             memcmp(route->method, method, method_len) == 0)) {
            *matched = route;
            *num_params = np;
            return 200;
        }

        /* HEAD falls back to GET routes */
        if (method_len == 4 && memcmp(method, "HEAD", 4) == 0 &&
            strlen(route->method) == 3 &&
            memcmp(route->method, "GET", 3) == 0) {
            *matched = route;
            *num_params = np;
            return 200;
        }
    }

    *matched = NULL;
    *num_params = 0;
    return path_matched ? 405 : 404;
}

void kl_router_free(KlRouter *r) {
    if (r->routes) {
        kl_free(r->alloc, r->routes, sizeof(KlRoute) * (size_t)r->capacity);
        r->routes = NULL;
    }
}
