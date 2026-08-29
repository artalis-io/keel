/**
 * resolver_cache.h: Caching DNS resolver decorator
 *
 * Wraps any KlResolver and caches successful results with configurable TTL.
 * Returns a KlResolver * that plugs directly into KlHttpClientConfig.resolver.
 *
 * Cache miss → delegates to inner resolver, stores result on completion.
 * Cache hit  → calls done_fn synchronously, returns immediately.
 * Errors are never cached; the next resolve retries via the inner resolver.
 */

#ifndef KEEL_RESOLVER_CACHE_H
#define KEEL_RESOLVER_CACHE_H

#include <stdint.h>
#include <keel/allocator.h>
#include <keel/resolver.h>
#ifdef __cplusplus
extern "C" {
#endif


#define KL_RESCACHE_DEFAULT_TTL_MS   60000
#define KL_RESCACHE_DEFAULT_CAPACITY 64

/*
 * Append-only config (see docs/contracts/compatibility.md): callers
 * zero-initialize and recompile per major version; every member is optional and
 * its zero/NULL value selects the built-in default. New members are appended
 * after capacity.
 */
typedef struct {
    uint64_t ttl_ms;      /**< Cache TTL in ms (0 = default 60s) */
    int      capacity;    /**< Max cache entries (0 = default 64) */
} KlResolverCacheConfig;

/**
 * @brief Create a caching resolver that wraps an inner resolver.
 *
 * The returned KlResolver * is the cache; destroy it via the vtable's
 * destroy function.  The inner resolver is borrowed (not owned); the
 * caller must destroy it separately after destroying the cache.
 *
 * @param inner  Inner resolver to delegate cache misses to (borrowed).
 * @param cfg    Config (NULL = defaults: 64 entries, 60 s TTL).
 * @param alloc  Allocator (borrowed).
 * @return KlResolver * on success, NULL on allocation failure.
 */
KlResolver *kl_resolver_cache_create(KlResolver *inner,
                                      const KlResolverCacheConfig *cfg,
                                      KlAllocator *alloc);

/**
 * @brief Evict all cached entries.
 */
void kl_resolver_cache_clear(KlResolver *resolver);

/**
 * @brief Return the number of occupied cache entries.
 */
int kl_resolver_cache_count(const KlResolver *resolver);

#ifdef __cplusplus
}
#endif

#endif /* KEEL_RESOLVER_CACHE_H */
