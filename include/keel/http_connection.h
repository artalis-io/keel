#ifndef KEEL_HTTP_CONNECTION_H
#define KEEL_HTTP_CONNECTION_H

#include <keel/allocator.h>      /* KlAllocator (pool shell) */
#include <keel/sockaddr.h>       /* KlSockAddr (peer accessor) */
#include <keel/http_response.h>  /* KlHttpResponse (response accessor) */
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief An HTTP server connection: an opaque, pool-owned borrowed handle.
 *
 * Application code receives a `KlHttpConn *` from kl_http_request_conn() (or a handler's request) and
 * uses the accessors below; it never constructs, sizes, or dereferences one. The concrete layout, the
 * connection-state enum, and the server/driver `kl_http_conn_*` functions are internal, in
 * src/protocols/http/http_conn_internal.h.
 */
typedef struct KlHttpConn KlHttpConn;

/**
 * @brief Server connection pool: a concrete shell embedded by value in KlHttpServer.
 *
 * This layout is public ONLY because the caller-owned KlHttpServer embeds a KlHttpConnPool by value;
 * its fields are visible SOLELY to permit the enclosing object's allocation and are NOT an application
 * contract. The pool is managed by the server through internal functions (http_conn_internal.h); do
 * not read or write these fields. It holds KlHttpConn by pointer, so it needs only the forward
 * declaration above (the connection layout stays opaque).
 */
typedef struct {
    KlHttpConn *conns;      /**< slot array (visible-for-allocation only) */
    int capacity;           /**< visible-for-allocation only */
    int active_count;       /**< visible-for-allocation only */
    int free_credits;       /**< visible-for-allocation only */
    KlHttpConn *free_list;  /**< visible-for-allocation only */
    KlAllocator *alloc;     /**< visible-for-allocation only */
} KlHttpConnPool;

/**
 * @brief Peer (client) address for a connection: stable borrowed-handle accessor.
 *
 * Returns a pointer to the connection's peer address (family KL_AF_UNSPEC when unavailable).
 * Ownership: borrowed (owned by the connection; do not free). NULL: returns NULL when @p c is NULL.
 */
const KlSockAddr *kl_http_conn_peer_addr(const KlHttpConn *c);

/**
 * @brief Response builder for a connection: stable borrowed-handle accessor.
 *
 * Returns the connection's `KlHttpResponse`, for async workflows that resume a suspended connection
 * and set its response outside the handler.
 *
 * Ownership: borrowed; the response is owned by the connection (pool storage), so the caller must not
 * free it. Lifetime: valid while the connection is live (the borrowed `KlHttpConn *` returned by
 * `kl_http_request_conn()`). Mutability: drive it through the `kl_http_response_*` API. NULL: returns
 * NULL when @p c is NULL (deterministic).
 */
KlHttpResponse       *kl_http_conn_response(KlHttpConn *c);
const KlHttpResponse *kl_http_conn_response_const(const KlHttpConn *c);

#ifdef __cplusplus
}
#endif

#endif
