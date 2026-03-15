/*
 * resolver_cache.c — Caching DNS resolver decorator
 *
 * Flat array with linear scan (same pattern as KlClientPool).
 * Cache sizes are small (default 64), so linear scan is cache-friendly.
 */

#include <keel/resolver_cache.h>
#include <keel/client.h>       /* KL_CLIENT_HOSTNAME_MAX */
#include <keel/connection.h>   /* kl_monotonic_ms */

#include <stdint.h>
#include <string.h>

/* ── Cache entry ─────────────────────────────────────────────────── */

typedef struct {
    char            host[KL_CLIENT_HOSTNAME_MAX];
    int             port;
    KlResolveResult result;
    uint64_t        deadline_ms;
    int             occupied;
} KlResCacheEntry;

/* ── Cache struct ────────────────────────────────────────────────── */

typedef struct KlResolverCache {
    KlResolver       base;       /* vtable — MUST be first for upcast */
    KlResolver      *inner;      /* borrowed */
    KlResCacheEntry *entries;
    int              capacity;
    int              count;
    uint64_t         ttl_ms;
    KlAllocator     *alloc;      /* borrowed */
} KlResolverCache;

/* ── Per-request handle ──────────────────────────────────────────── */

/*
 * Sync-completion sentinel pattern:
 *
 * The inner resolver's resolve() may call done_fn synchronously (before
 * returning).  When this happens, inner_done_fn fires while we're still
 * inside cache_resolve.  Without guards, inner_done_fn would free the
 * KlResCacheReq while cache_resolve still holds a pointer to it.
 *
 * Solution: cache_resolve sets in_resolve=1 before calling inner->resolve
 * and clears it after.  inner_done_fn checks in_resolve:
 *   - If 1 (sync): sets completed=1 and does NOT free — cache_resolve
 *     will see completed and knows the callback already fired.
 *   - If 0 (async): frees the request — it's unreferenced after callback.
 *
 * This is the canonical pattern for decorators wrapping a KlResolver.
 * Any new decorator that forwards to an inner resolver must replicate it.
 */
typedef struct {
    KlResolveReq     base;
    KlResolveReq    *inner_req;   /* NULL if cache hit */
    KlResolveDoneFn  user_done;
    void            *user_data;
    char             host[KL_CLIENT_HOSTNAME_MAX];
    int              port;
    KlResolverCache *cache;
    int              in_resolve;  /* 1 while inside cache_resolve */
    int              completed;   /* set if inner completed synchronously */
} KlResCacheReq;

/* ── Cache lookup / insert ───────────────────────────────────────── */

static const KlResolveResult *cache_lookup(KlResolverCache *c,
                                            const char *host, int port)
{
    uint64_t now = kl_monotonic_ms();
    for (int i = 0; i < c->capacity; i++) {
        KlResCacheEntry *e = &c->entries[i];
        if (e->occupied && e->port == port &&
            strcmp(e->host, host) == 0) {
            if (e->deadline_ms > now)
                return &e->result;
            /* Expired — mark free */
            e->occupied = 0;
            c->count--;
            return NULL;
        }
    }
    return NULL;
}

static void cache_insert(KlResolverCache *c, const char *host, int port,
                           const KlResolveResult *result)
{
    uint64_t now = kl_monotonic_ms();
    uint64_t deadline = (c->ttl_ms > UINT64_MAX / 2) ? UINT64_MAX
                                                       : now + c->ttl_ms;

    /* 1. Update existing entry for same (host, port) */
    for (int i = 0; i < c->capacity; i++) {
        KlResCacheEntry *e = &c->entries[i];
        if (e->occupied && e->port == port &&
            strcmp(e->host, host) == 0) {
            e->result = *result;
            e->deadline_ms = deadline;
            return;
        }
    }

    /* 2. Find a free slot */
    for (int i = 0; i < c->capacity; i++) {
        if (!c->entries[i].occupied) {
            KlResCacheEntry *e = &c->entries[i];
            memcpy(e->host, host, strlen(host) + 1);
            e->port = port;
            e->result = *result;
            e->deadline_ms = deadline;
            e->occupied = 1;
            c->count++;
            return;
        }
    }

    /* 3. Evict first expired entry */
    for (int i = 0; i < c->capacity; i++) {
        KlResCacheEntry *e = &c->entries[i];
        if (e->deadline_ms <= now) {
            memcpy(e->host, host, strlen(host) + 1);
            e->port = port;
            e->result = *result;
            e->deadline_ms = deadline;
            /* count stays the same — replacing occupied entry */
            return;
        }
    }

    /* 4. Evict entry closest to expiry */
    int victim = 0;
    uint64_t min_deadline = c->entries[0].deadline_ms;
    for (int i = 1; i < c->capacity; i++) {
        if (c->entries[i].deadline_ms < min_deadline) {
            min_deadline = c->entries[i].deadline_ms;
            victim = i;
        }
    }
    KlResCacheEntry *e = &c->entries[victim];
    memcpy(e->host, host, strlen(host) + 1);
    e->port = port;
    e->result = *result;
    e->deadline_ms = deadline;
    /* count stays the same — replacing occupied entry */
}

/* ── Inner resolver completion callback ──────────────────────────── */

static void inner_done_fn(KlResolveReq *req, const KlResolveResult *result,
                            int error, void *user_data)
{
    KlResCacheReq *cr = user_data;

    /* Cache successful results only */
    if (error == 0 && result)
        cache_insert(cr->cache, cr->host, cr->port, result);

    (void)req;

    /* Forward to caller */
    cr->user_done(&cr->base, result, error, cr->user_data);

    if (cr->in_resolve) {
        /* Sync completion: still inside cache_resolve. Don't free —
         * the returned handle will be freed later via cancel(). */
        cr->completed = 1;
    } else {
        /* Async completion: request is unreferenced after callback. */
        cr->cache->alloc->free(cr->cache->alloc, cr, sizeof(*cr));
    }
}

/* ── Vtable: resolve ─────────────────────────────────────────────── */

static KlResolveReq *cache_resolve(KlResolver *self, KlEventCtx *ctx,
                                     const char *host, int port,
                                     KlResolveDoneFn done_fn, void *user_data)
{
    KlResolverCache *c = (KlResolverCache *)self;

    if (!host || !done_fn)
        return NULL;

    size_t host_len = strlen(host);
    if (host_len == 0 || host_len >= KL_CLIENT_HOSTNAME_MAX)
        return NULL;

    /* Allocate per-request handle */
    KlResCacheReq *cr = c->alloc->malloc(c->alloc, sizeof(*cr));
    if (!cr)
        return NULL;
    memset(cr, 0, sizeof(*cr));

    cr->base.resolver = self;
    cr->user_done = done_fn;
    cr->user_data = user_data;
    cr->cache = c;
    cr->port = port;
    memcpy(cr->host, host, host_len + 1);

    /* Check cache */
    const KlResolveResult *cached = cache_lookup(c, host, port);
    if (cached) {
        cr->inner_req = NULL;
        /* Call done_fn synchronously with cached result */
        done_fn(&cr->base, cached, 0, user_data);
        return &cr->base;
    }

    /* Cache miss — delegate to inner resolver.
     * Set in_resolve flag so inner_done_fn defers freeing if the inner
     * resolver completes synchronously (callback fires inside resolve). */
    cr->in_resolve = 1;
    KlResolveReq *inner = c->inner->resolve(c->inner, ctx, host, port,
                                              inner_done_fn, cr);
    cr->in_resolve = 0;

    if (!inner) {
        /* inner_done_fn may have already run (sync completion) and called
         * done_fn before we return NULL.  This is contract-compatible:
         * client.c handles the same pattern (test_client.c:184-210).
         * Free cr in both cases — completed or not. */
        c->alloc->free(c->alloc, cr, sizeof(*cr));
        return NULL;
    }

    cr->inner_req = inner;
    return &cr->base;
}

/* ── Vtable: cancel ──────────────────────────────────────────────── */

static void cache_cancel(KlResolveReq *req)
{
    KlResCacheReq *cr = (KlResCacheReq *)req;
    KlAllocator *alloc = cr->cache->alloc;

    if (cr->inner_req && !cr->completed)
        cr->cache->inner->cancel(cr->inner_req);

    alloc->free(alloc, cr, sizeof(*cr));
}

/* ── Vtable: destroy ─────────────────────────────────────────────── */

static void cache_destroy(KlResolver *self)
{
    KlResolverCache *c = (KlResolverCache *)self;
    KlAllocator *alloc = c->alloc;

    alloc->free(alloc, c->entries,
                (size_t)c->capacity * sizeof(KlResCacheEntry));
    alloc->free(alloc, c, sizeof(*c));
}

/* ── Public API ──────────────────────────────────────────────────── */

KlResolver *kl_resolver_cache_create(KlResolver *inner,
                                      const KlResolverCacheConfig *cfg,
                                      KlAllocator *alloc)
{
    if (!inner || !alloc)
        return NULL;

    int capacity = KL_RESCACHE_DEFAULT_CAPACITY;
    uint64_t ttl_ms = KL_RESCACHE_DEFAULT_TTL_MS;

    if (cfg) {
        if (cfg->capacity > 0)
            capacity = cfg->capacity;
        if (cfg->ttl_ms > 0)
            ttl_ms = cfg->ttl_ms;
    }

    /* Overflow guard: capacity * sizeof(entry) must not wrap */
    if ((size_t)capacity > SIZE_MAX / sizeof(KlResCacheEntry))
        return NULL;

    KlResolverCache *c = alloc->malloc(alloc, sizeof(*c));
    if (!c)
        return NULL;
    memset(c, 0, sizeof(*c));

    c->entries = alloc->malloc(alloc, (size_t)capacity * sizeof(KlResCacheEntry));
    if (!c->entries) {
        alloc->free(alloc, c, sizeof(*c));
        return NULL;
    }
    memset(c->entries, 0, (size_t)capacity * sizeof(KlResCacheEntry));

    c->base.resolve = cache_resolve;
    c->base.cancel  = cache_cancel;
    c->base.destroy = cache_destroy;
    c->inner    = inner;
    c->capacity = capacity;
    c->count    = 0;
    c->ttl_ms   = ttl_ms;
    c->alloc    = alloc;

    return &c->base;
}

void kl_resolver_cache_clear(KlResolver *resolver)
{
    if (!resolver)
        return;
    KlResolverCache *c = (KlResolverCache *)resolver;
    for (int i = 0; i < c->capacity; i++)
        c->entries[i].occupied = 0;
    c->count = 0;
}

int kl_resolver_cache_count(const KlResolver *resolver)
{
    if (!resolver)
        return 0;
    const KlResolverCache *c = (const KlResolverCache *)resolver;
    return c->count;
}
