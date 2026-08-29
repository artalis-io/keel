/*
 * http_client_pool_internal.h: INTERNAL. The concrete KlHttpClientPoolEntry layout.
 *
 * KlHttpClientPoolEntry is opaque on the public surface (forward-declared in
 * <keel/http_client_pool.h>; KlHttpClientPool holds it by pointer). Only the client-pool
 * implementation (src/protocols/http/http_client_pool.c) needs the layout; include this ONLY from
 * there and from explicitly-justified white-box tests. No public header, other protocol TU,
 * substrate, or integration needs it.
 */
#ifndef KEEL_SRC_HTTP_CLIENT_POOL_INTERNAL_H
#define KEEL_SRC_HTTP_CLIENT_POOL_INTERNAL_H

#include <keel/http_client_pool.h>   /* KlHttpClientPoolEntry forward decl + KlHttpClientPool */
#include <keel/http_client.h>        /* KL_HTTP_CLIENT_HOSTNAME_MAX */
#include <keel/handle.h>             /* KlSocketHandle */
#include <keel/tls.h>                /* KlTls */
#include <stdint.h>

struct KlHttpClientPoolEntry {
    char     host[KL_HTTP_CLIENT_HOSTNAME_MAX]; /* NUL-terminated key */
    int      port;
    int      is_tls;
    char     proxy_host[KL_HTTP_CLIENT_HOSTNAME_MAX]; /* "" = direct connection */
    int      proxy_port;                          /* 0 = direct connection */
    KlSocketHandle fd;            /* -1 = free slot */
    KlTls   *tls;
    uint64_t idle_since_ms; /* kl_monotonic_ms() when returned */
    int64_t  timer_id;      /* idle timer (-1 = none) */
    struct KlHttpClientPool *pool; /* back-pointer for timer callback */
};

#endif /* KEEL_SRC_HTTP_CLIENT_POOL_INTERNAL_H */
