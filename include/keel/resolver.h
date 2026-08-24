/**
 * resolver.h: Pluggable async DNS resolver vtable
 *
 * When set in KlHttpClientConfig, the async client uses this for non-blocking
 * name resolution. When NULL, falls back to sync getaddrinfo (default).
 *
 * Users can plug in c-ares, a thread-pool wrapper, or a custom implementation.
 */

#ifndef KEEL_RESOLVER_H
#define KEEL_RESOLVER_H

#include <keel/allocator.h>
#include <keel/sockaddr.h>

/* Forward declarations */
typedef struct KlEventCtx KlEventCtx;
typedef struct KlResolver KlResolver;
typedef struct KlResolveReq KlResolveReq;

/** @brief Maximum addresses returned per resolution. */
#define KL_RESOLVE_MAX_ADDRS 8

/**
 * @brief Result passed to the completion callback.
 *
 * Carries an ordered address list (preferred first). Each address is a
 * self-describing KlSockAddr (family via kl_sockaddr_family(), no separate
 * length); `addrs[0]` is the primary/preferred address. `ai_socktype` and
 * `ai_protocol` are shared across all entries.
 */
typedef struct {
    KlSockAddr addrs[KL_RESOLVE_MAX_ADDRS];  /**< Resolved addresses, preferred first */
    int naddrs;                              /**< Number of addresses (>= 1 on success) */
    int ai_socktype;                         /**< Socket type (shared) */
    int ai_protocol;                         /**< Protocol (shared) */
} KlResolveResult;

/** @brief Completion callback: called on the event loop thread. */
typedef void (*KlResolveDoneFn)(KlResolveReq *req, const KlResolveResult *result,
                                 int error, void *user_data);

/** @brief Opaque per-request handle: resolver implementation allocates. */
struct KlResolveReq {
    KlResolver *resolver;  /**< Back-pointer to owning resolver */
};

/** @brief Async DNS resolver vtable. */
struct KlResolver {
    /**
     * Start async name resolution. Must not block.
     * Returns a KlResolveReq handle (resolver-owned), or NULL on error.
     * Calls done_fn on the event loop thread when resolution completes.
     *
     * Sync completion: resolve() MAY call done_fn synchronously (inside
     * the call, before returning).  Decorators that wrap another resolver
     * must handle this; the inner resolver's done_fn may fire before
     * inner->resolve() returns.  Use an in_resolve/completed sentinel
     * to detect sync completion and defer freeing the per-request handle.
     * See resolver_cache.c for the canonical implementation of this pattern.
     */
    KlResolveReq *(*resolve)(KlResolver *self, KlEventCtx *ctx,
                              const char *host, int port,
                              KlResolveDoneFn done_fn, void *user_data);

    /** Cancel an in-flight request. May be called during shutdown. */
    void (*cancel)(KlResolveReq *req);

    /** Destroy the resolver (free any shared state). */
    void (*destroy)(KlResolver *self);
};

#endif /* KEEL_RESOLVER_H */
