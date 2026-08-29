/*
 * resolver_cache.c: Caching DNS resolver decorator
 *
 * Flat array with linear scan (same pattern as KlHttpClientPool).
 * Cache sizes are small (default 64), so linear scan is cache-friendly.
 */

#include <keel/resolver_cache.h>
#include <keel/allocator.h>    /* kl_malloc / kl_free wrappers */
#include <keel/clock.h>            /* kl_monotonic_ms */
#include "allocator_validate.h"

/* Private DNS hostname bound for cached entries (resolver-owned; not public config). */
#define KL_RESOLVER_CACHE_HOSTNAME_MAX 256

#include <stdint.h>
#include <string.h>

/* ── Cache entry ─────────────────────────────────────────────────── */

typedef struct {
    char            host[KL_RESOLVER_CACHE_HOSTNAME_MAX];
    int             port;
    KlResolveResult result;
    uint64_t        deadline_ms;
    int             occupied;
} KlResCacheEntry;

/* ── Cache struct ────────────────────────────────────────────────── */

typedef struct KlResolverCache {
    KlResolver       base;       /* vtable: MUST be first for upcast */
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
 *   - If 1 (sync): sets completed=1 and does NOT free; cache_resolve
 *     will see completed and knows the callback already fired.
 *   - If 0 (async): frees the request; it's unreferenced after callback.
 *
 * This is the canonical pattern for decorators wrapping a KlResolver.
 * Any new decorator that forwards to an inner resolver must replicate it.
 */
typedef struct {
    KlResolveReq     base;
    KlResolveReq    *inner_req;   /* NULL if cache hit */
    KlResolveDoneFn  user_done;
    void            *user_data;
    char             host[KL_RESOLVER_CACHE_HOSTNAME_MAX];
    int              port;
    KlResolverCache *cache;
    int              in_resolve;     /* 1 while inside cache_resolve */
    int              completed;      /* set when inner has produced a result */
    int              in_user_done;   /* 1 while user_done is on the stack */
    int              cancel_deferred;/* 1 if cancel() was called during user_done */
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
            /* Expired: mark free and continue scanning.  The
             * cache_insert invariant says there's at most one entry
             * per (host, port), so this loop terminates with the
             * single match either returned or evicted; but defensive
             * continuation hardens against any future invariant
             * break (e.g. concurrent decorator). */
            e->occupied = 0;
            c->count--;
            /* fall through to break: only one entry per (host, port) */
            return NULL;
        }
    }
    return NULL;
}

static void cache_insert(KlResolverCache *c, const char *host, int port,
                           const KlResolveResult *result)
{
    /* Self-defend the fixed host[] buffer: callers already bound host length, but
     * keep the memcpy(strlen+1) copies below safe regardless of caller. */
    if (strlen(host) >= KL_RESOLVER_CACHE_HOSTNAME_MAX)
        return;

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
            /* count stays the same: replacing occupied entry */
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
    /* count stays the same: replacing occupied entry */
}

/* ── Inner resolver completion callback ──────────────────────────── */

static void inner_done_fn(KlResolveReq *req, const KlResolveResult *result,
                            int error, void *user_data)
{
    KlResCacheReq *cr = user_data;
    (void)req;

    /* Cache successful results only.  Done BEFORE user_done because
     * user_done may free cr via cache_cancel. */
    if (error == 0 && result)
        cache_insert(cr->cache, cr->host, cr->port, result);

    /* Snapshot the sync flag + allocator BEFORE user_done.  The user
     * MAY call cache_cancel(&cr->base) from inside their done handler
     * (common when the calling stream is being torn down).  Without
     * snapshots, any post-callback read of cr would be use-after-free.
     *
     * Set completed=1 before user_done so a re-entrant cancel sees a
     * coherent state (no double-cancel of inner).  Set in_user_done=1
     * so cache_cancel knows to DEFER the free until we return; if it
     * freed now, our post-callback branch below would touch dead
     * memory. */
    int was_sync = cr->in_resolve;
    KlAllocator *alloc = cr->cache->alloc;
    cr->completed = 1;
    cr->in_user_done = 1;

    cr->user_done(&cr->base, result, error, cr->user_data);

    cr->in_user_done = 0;

    if (was_sync) {
        /* Sync completion: we're nested inside cache_resolve, which
         * still needs to read cr->* after we return (to set
         * cr->inner_req, etc.).  Never free cr here on the sync path
         * (cache_resolve owns the post-call free decision; see the
         * cancel_deferred check there). */
        return;
    }

    /* Async completion: cr is unreferenced after callback unless the
     * user already cancelled (which deferred to us). Free unconditionally
     * (cancel_deferred=1 means the user called cancel but we held it
     * off; the free that cancel would've done happens here, exactly once. */
    kl_free(alloc, cr, sizeof(*cr));
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
    if (host_len == 0 || host_len >= KL_RESOLVER_CACHE_HOSTNAME_MAX)
        return NULL;

    /* Allocate per-request handle */
    KlResCacheReq *cr = kl_malloc(c->alloc, sizeof(*cr));
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
        /* Cache hit: synthesise a sync completion through inner_done_fn
         * so the re-entrant-cancel-safety machinery (in_user_done +
         * cancel_deferred) covers this path too. */
        cr->in_resolve = 1;
        cr->in_user_done = 1;
        cr->completed = 1;
        done_fn(&cr->base, cached, 0, user_data);
        cr->in_user_done = 0;
        cr->in_resolve = 0;
        if (cr->cancel_deferred) {
            /* User cancelled inside the synchronous callback.  Inner
             * never ran; just free. */
            kl_free(c->alloc, cr, sizeof(*cr));
            return NULL;
        }
        return &cr->base;
    }

    /* Cache miss: delegate to inner resolver.
     * Set in_resolve flag so inner_done_fn defers freeing if the inner
     * resolver completes synchronously (callback fires inside resolve). */
    cr->in_resolve = 1;
    KlResolveReq *inner = c->inner->resolve(c->inner, ctx, host, port,
                                              inner_done_fn, cr);
    cr->in_resolve = 0;

    if (!inner) {
        /* inner_done_fn may have already run (sync completion) and called
         * done_fn before we return NULL.  Free cr regardless; the
         * inner_done_fn sync path is contracted to NOT free on its own. */
        kl_free(c->alloc, cr, sizeof(*cr));
        return NULL;
    }

    cr->inner_req = inner;

    if (cr->cancel_deferred) {
        /* User called cache_cancel() from inside the synchronous
         * done_fn that fired during inner->resolve().  cr is alive
         * because inner_done_fn's sync branch deferred the free to
         * us.  Honor the cancel now: inner has already completed
         * (we're past inner->resolve), so skip inner->cancel; just
         * free cr and return NULL; the user has no need for a
         * handle to an already-dead request. */
        kl_free(c->alloc, cr, sizeof(*cr));
        return NULL;
    }
    return &cr->base;
}

/* ── Vtable: cancel ──────────────────────────────────────────────── */

static void cache_cancel(KlResolveReq *req)
{
    KlResCacheReq *cr = (KlResCacheReq *)req;
    KlAllocator *alloc = cr->cache->alloc;

    /* Re-entrant cancel from inside the user's done_fn (which is on
     * the call stack above us).  Freeing now would UAF on
     * inner_done_fn's post-callback branch (which still needs to read
     * cr->* before returning), and on cache_resolve's post-resolve
     * writes if we're on the sync path.  Mark the deferral; the call
     * that's holding cr alive will see cancel_deferred and free. */
    if (cr->in_user_done) {
        cr->cancel_deferred = 1;
        return;
    }

    if (cr->inner_req && !cr->completed)
        cr->cache->inner->cancel(cr->inner_req);

    kl_free(alloc, cr, sizeof(*cr));
}

/* ── Vtable: destroy ─────────────────────────────────────────────── */

static void cache_destroy(KlResolver *self)
{
    KlResolverCache *c = (KlResolverCache *)self;
    KlAllocator *alloc = c->alloc;

    kl_free(alloc, c->entries,
            (size_t)c->capacity * sizeof(KlResCacheEntry));
    kl_free(alloc, c, sizeof(*c));
}

/* ── Public API ──────────────────────────────────────────────────── */

KlResolver *kl_resolver_cache_create(KlResolver *inner,
                                      const KlResolverCacheConfig *cfg,
                                      KlAllocator *alloc)
{
    if (!inner || !kl_allocator_ops_valid(alloc))
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

    KlResolverCache *c = kl_malloc(alloc, sizeof(*c));
    if (!c)
        return NULL;
    memset(c, 0, sizeof(*c));

    c->entries = kl_malloc(alloc, (size_t)capacity * sizeof(KlResCacheEntry));
    if (!c->entries) {
        kl_free(alloc, c, sizeof(*c));
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
