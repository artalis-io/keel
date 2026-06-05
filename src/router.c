#include <keel/router.h>
#include <string.h>
#include <limits.h>

#define KL_ROUTER_INIT_CAP 16

int kl_router_init(KlRouter *r, KlAllocator *alloc) {
    r->alloc = alloc;
    r->count = 0;
    r->capacity = KL_ROUTER_INIT_CAP;
    r->middleware = NULL;
    r->mw_count = 0;
    r->mw_capacity = 0;
    r->post_middleware = NULL;
    r->post_mw_count = 0;
    r->post_mw_capacity = 0;
    r->routes = kl_malloc(alloc, sizeof(KlRoute) * (size_t)r->capacity);
    return r->routes ? 0 : -1;
}

int kl_router_add(KlRouter *r, const char *method, const char *pattern,
                  KlHandler handler, void *user_data,
                  KlBodyReaderFactory body_reader) {
    if (!r || !method || !pattern) return -1;
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
        .method_len = strlen(method),
        .pattern_len = strlen(pattern),
        .handler = handler,
        .user_data = user_data,
        .body_reader = body_reader,
    };
    r->count++;
    return 0;
}

int kl_router_add_streaming(KlRouter *r, const char *method, const char *pattern,
                             KlHandler handler, void *user_data,
                             KlBodyReaderFactory body_reader) {
    /* Streaming handlers need a body reader to pump on_data into. */
    if (!body_reader) return -1;
    int rc = kl_router_add(r, method, pattern, handler, user_data, body_reader);
    if (rc < 0) return rc;
    /* kl_router_add appended to slot count-1. Mark it streaming. */
    r->routes[r->count - 1].streaming_handler = 1;
    return 0;
}

int kl_router_add_streaming_async(KlRouter *r, const char *method,
                                    const char *pattern,
                                    KlHandler handler, void *user_data,
                                    KlBodyReaderFactory body_reader) {
    if (!body_reader) return -1;
    int rc = kl_router_add(r, method, pattern, handler, user_data, body_reader);
    if (rc < 0) return rc;
    /* streaming_handler enables the early-dispatch / mid-stream-exit
     * machinery; streaming_async additionally tells conn_dispatch_request
     * to invoke the handler BEFORE feeding leftover body bytes (so the
     * handler can park on NEED_DATA and be resumed by on_data — both
     * for the leftover AND subsequent socket reads). */
    r->routes[r->count - 1].streaming_handler = 1;
    r->routes[r->count - 1].streaming_async   = 1;
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
                (*num_params)++;
            }
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

/* ── Middleware ──────────────────────────────────────────────────────── */

#define KL_MW_INIT_CAP 8

static int match_middleware_pattern(const char *method, size_t method_len,
                                    const char *mw_method, size_t mw_method_len,
                                    const char *pattern, size_t pat_len,
                                    const char *path, size_t path_len) {
    /* Method check */
    if (mw_method[0] != '*') {
        int ok = (mw_method_len == method_len &&
                  memcmp(mw_method, method, mw_method_len) == 0);
        /* HEAD falls back to GET */
        if (!ok && method_len == 4 && memcmp(method, "HEAD", 4) == 0)
            ok = (mw_method_len == 3 && memcmp(mw_method, "GET", 3) == 0);
        if (!ok) return 0;
    }

    /* Prefix match: pattern ends with slash-star */
    if (pat_len >= 2 && pattern[pat_len - 2] == '/' &&
        pattern[pat_len - 1] == '*') {
        size_t prefix_len = pat_len - 2;
        if (prefix_len == 0) return 1;  /* matches everything */
        if (path_len < prefix_len) return 0;
        if (memcmp(pattern, path, prefix_len) != 0) return 0;
        return (path_len == prefix_len || path[prefix_len] == '/');
    }

    /* Exact match */
    return (pat_len == path_len && memcmp(pattern, path, pat_len) == 0);
}

int kl_router_use(KlRouter *r, const char *method, const char *pattern,
                  KlMiddleware fn, void *user_data) {
    if (!r || !method || !pattern || !fn) return -1;
    if (r->mw_count >= r->mw_capacity) {
        int new_cap;
        if (r->mw_capacity == 0) {
            new_cap = KL_MW_INIT_CAP;
        } else {
            if (r->mw_capacity > INT_MAX / 2) return -1;
            new_cap = r->mw_capacity * 2;
        }
        size_t old_size = sizeof(KlMiddlewareEntry) * (size_t)r->mw_capacity;
        size_t new_size = sizeof(KlMiddlewareEntry) * (size_t)new_cap;
        KlMiddlewareEntry *new_mw = kl_realloc(r->alloc, r->middleware,
                                                old_size, new_size);
        if (!new_mw) return -1;
        r->middleware = new_mw;
        r->mw_capacity = new_cap;
    }

    r->middleware[r->mw_count] = (KlMiddlewareEntry){
        .method = method,
        .pattern = pattern,
        .method_len = strlen(method),
        .pattern_len = strlen(pattern),
        .fn = fn,
        .user_data = user_data,
    };
    r->mw_count++;
    return 0;
}

int kl_router_run_middleware(KlRouter *r, KlRequest *req, KlResponse *res) {
    for (int i = 0; i < r->mw_count; i++) {
        KlMiddlewareEntry *mw = &r->middleware[i];
        if (match_middleware_pattern(req->method, req->method_len,
                                    mw->method, mw->method_len,
                                    mw->pattern, mw->pattern_len,
                                    req->path, req->path_len)) {
            int rc = mw->fn(req, res, mw->user_data);
            if (rc != 0) return rc;
        }
    }
    return 0;
}

int kl_router_use_post(KlRouter *r, const char *method, const char *pattern,
                       KlMiddleware fn, void *user_data) {
    if (!r || !method || !pattern || !fn) return -1;
    if (r->post_mw_count >= r->post_mw_capacity) {
        int new_cap;
        if (r->post_mw_capacity == 0) {
            new_cap = KL_MW_INIT_CAP;
        } else {
            if (r->post_mw_capacity > INT_MAX / 2) return -1;
            new_cap = r->post_mw_capacity * 2;
        }
        size_t old_size = sizeof(KlMiddlewareEntry) * (size_t)r->post_mw_capacity;
        size_t new_size = sizeof(KlMiddlewareEntry) * (size_t)new_cap;
        KlMiddlewareEntry *new_mw = kl_realloc(r->alloc, r->post_middleware,
                                                old_size, new_size);
        if (!new_mw) return -1;
        r->post_middleware = new_mw;
        r->post_mw_capacity = new_cap;
    }

    r->post_middleware[r->post_mw_count] = (KlMiddlewareEntry){
        .method = method,
        .pattern = pattern,
        .method_len = strlen(method),
        .pattern_len = strlen(pattern),
        .fn = fn,
        .user_data = user_data,
    };
    r->post_mw_count++;
    return 0;
}

int kl_router_run_post_middleware(KlRouter *r, KlRequest *req, KlResponse *res) {
    for (int i = 0; i < r->post_mw_count; i++) {
        KlMiddlewareEntry *mw = &r->post_middleware[i];
        if (match_middleware_pattern(req->method, req->method_len,
                                    mw->method, mw->method_len,
                                    mw->pattern, mw->pattern_len,
                                    req->path, req->path_len)) {
            int rc = mw->fn(req, res, mw->user_data);
            if (rc != 0) return rc;
        }
    }
    return 0;
}

/* ── Synthetic request dispatch ─────────────────────────────────────── */

int kl_router_dispatch_synthetic(KlRouter *r, KlRequest *req,
                                  KlResponse *res, int run_middleware) {
    if (!r || !req || !res) return -1;

    /* Match route. params live on the request; the caller doesn't have
     * to pre-fill them. */
    KlRoute *matched = NULL;
    int num_params = 0;
    int match_status = kl_router_match(r, req->method, req->method_len,
                                       req->path, req->path_len,
                                       &matched, req->params, &num_params);
    /* Defensive clamp: kl_router_match already bounds writes into params
     * by KL_MAX_PARAMS (see match_path) — repeating the clamp on the
     * count we expose protects req->num_params against any future
     * regression in the matcher and costs nothing on the hot path. */
    if (num_params < 0) num_params = 0;
    if (num_params > KL_MAX_PARAMS) num_params = KL_MAX_PARAMS;
    req->num_params = num_params;

    /* No-middleware fast path for a miss — caller decides what to do
     * with the status (e.g. a test that explicitly asserts 404 doesn't
     * want middleware to run). */
    if (!run_middleware && (match_status != 200 || !matched))
        return match_status;

    /* Pre-body middleware. Short-circuit returns the middleware's
     * return value so the caller can distinguish handler-status from
     * middleware-short-circuit. */
    if (run_middleware) {
        int mw_rc = kl_router_run_middleware(r, req, res);
        if (mw_rc != 0) return mw_rc;

        mw_rc = kl_router_run_post_middleware(r, req, res);
        if (mw_rc != 0) return mw_rc;
    }

    /* Dispatch handler if matched; otherwise stamp the match status. */
    if (match_status == 200 && matched) {
        matched->handler(req, res, matched->user_data);
        return 200;
    }
    kl_response_status(res, match_status);
    return match_status;
}

/* ── Route matching ─────────────────────────────────────────────────── */

int kl_router_match(KlRouter *r, const char *method, size_t method_len,
                    const char *path, size_t path_len,
                    KlRoute **matched, KlParam *params, int *num_params) {
    int path_matched = 0;

    for (int i = 0; i < r->count; i++) {
        KlRoute *route = &r->routes[i];
        int np = 0;

        if (!match_path(route->pattern, route->pattern_len,
                        path, path_len, params, &np))
            continue;

        path_matched = 1;

        /* Check method */
        if (route->method[0] == '*' ||
            (route->method_len == method_len &&
             memcmp(route->method, method, method_len) == 0)) {
            *matched = route;
            *num_params = np;
            return 200;
        }

        /* HEAD falls back to GET routes */
        if (method_len == 4 && memcmp(method, "HEAD", 4) == 0 &&
            route->method_len == 3 &&
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
    if (r->middleware) {
        kl_free(r->alloc, r->middleware,
                sizeof(KlMiddlewareEntry) * (size_t)r->mw_capacity);
        r->middleware = NULL;
    }
    if (r->post_middleware) {
        kl_free(r->alloc, r->post_middleware,
                sizeof(KlMiddlewareEntry) * (size_t)r->post_mw_capacity);
        r->post_middleware = NULL;
    }
}
