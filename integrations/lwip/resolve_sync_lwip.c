/*
 * resolve_sync_lwip.c — blocking name resolution over lwIP's own resolver.
 *
 * The lwIP counterpart of src/resolve_sync.c: it wraps lwip_getaddrinfo (not the
 * host getaddrinfo) and returns Keel-neutral KlSockAddr. Link this object AHEAD of
 * a stock libkeel.a so it satisfies kl_resolve_sync before the archive's host
 * version is pulled — that's how the Keel HTTP CLIENT resolves names/literals on a
 * pure lwIP target (no host libc resolver). Numeric literals work without LWIP_DNS;
 * name resolution additionally needs LWIP_DNS=1 in lwipopts.h.
 *
 * Bring-your-own lwIP (LWIP_DIR); see integrations/lwip/README.md.
 */
#define KEEL_PLATFORM_LWIP 1
#include "resolve_sync.h"      /* kl_resolve_sync decl + KlSockAddr */

#include "lwip/sockets.h"
#include "lwip/netdb.h"        /* lwip_getaddrinfo / lwip_freeaddrinfo */

#include <stdio.h>
#include <string.h>

int kl_resolve_sync(const char *host, uint16_t port, int socktype,
                    KlSockAddr *out, int max, int *n) {
    if (!host || !out || max <= 0 || !n)
        return -1;

    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = socktype;

    struct addrinfo *res = NULL;
    if (lwip_getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
        return -1;

    int cnt = 0;
    for (struct addrinfo *ai = res; ai && cnt < max; ai = ai->ai_next) {
        const struct sockaddr *sa = ai->ai_addr;
        if (sa->sa_family == AF_INET) {
            const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
            uint8_t ip[4];
            memcpy(ip, &in->sin_addr, 4);
            if (kl_sockaddr_from_ipv4(&out[cnt], ip, lwip_ntohs(in->sin_port)) == 0)
                cnt++;
        }
#if LWIP_IPV6
        else if (sa->sa_family == AF_INET6) {
            const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
            uint8_t ip[16];
            memcpy(ip, &in6->sin6_addr, 16);
            if (kl_sockaddr_from_ipv6(&out[cnt], ip, lwip_ntohs(in6->sin6_port), 0) == 0)
                cnt++;
        }
#endif
    }
    lwip_freeaddrinfo(res);

    if (cnt == 0)
        return -1;
    *n = cnt;
    return 0;
}
