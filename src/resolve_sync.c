/*
 * resolve_sync.c — POSIX/Winsock blocking name resolution (getaddrinfo) that
 * returns Keel-neutral KlSockAddr. This is the platform boundary that keeps
 * getaddrinfo + struct sockaddr out of the protocol client TUs (see
 * resolve_sync.h). A foreign stack (lwIP) swaps in its own resolve_sync_lwip.c.
 */
#include "resolve_sync.h"
#include "sockcompat.h"        /* getaddrinfo / struct addrinfo (POSIX + Winsock) */
#include "sockaddr_native.h"   /* kl_sockaddr_from_native */

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
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
        return -1;

    int cnt = 0;
    for (struct addrinfo *ai = res; ai && cnt < max; ai = ai->ai_next) {
        if (kl_sockaddr_from_native(&out[cnt], ai->ai_addr,
                                    (socklen_t)ai->ai_addrlen) == 0)
            cnt++;
    }
    freeaddrinfo(res);

    if (cnt == 0)
        return -1;
    *n = cnt;
    return 0;
}
