#ifndef KEEL_SRC_RESOLVE_SYNC_H
#define KEEL_SRC_RESOLVE_SYNC_H

/*
 * resolve_sync.h — blocking name resolution, KlSockAddr out.
 *
 * The ONE place the protocol clients' synchronous / Happy-Eyeballs-fallback path
 * touches getaddrinfo. Kept in a platform TU (resolve_sync.c on POSIX/Winsock; a
 * resolve_sync_lwip.c would wrap lwip_getaddrinfo) so the protocol clients (HTTP/1.1, HTTP/2, WebSocket) name NO platform
 * struct sockaddr and pull no <netdb.h> —
 * they speak KlSockAddr only. Address plumbing above the seam is neutral.
 *
 * INTERNAL header — not installed, no ABI commitment.
 */

#include <keel/sockaddr.h>
#include <keel/resolver.h>   /* KL_RESOLVE_MAX_ADDRS (caller array sizing) */
#include <stdint.h>

/*
 * Resolve `host` (name or numeric) + host-order `port` to up to `max` addresses,
 * writing them into `out` and the count into `*n`. `socktype` is the getaddrinfo
 * socket type (SOCK_STREAM for HTTP). Returns 0 on success (*n >= 1), -1 on
 * failure (no addresses / lookup error).
 */
int kl_resolve_sync(const char *host, uint16_t port, int socktype,
                    KlSockAddr *out, int max, int *n);

#endif /* KEEL_SRC_RESOLVE_SYNC_H */
