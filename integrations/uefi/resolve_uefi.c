/*
 * resolve_uefi.c — numeric-only kl_resolve_sync for the freestanding UEFI client.
 * See resolve_uefi.h. No libc, no getaddrinfo — a dotted-quad IPv4 literal parser.
 */

#include "resolve_uefi.h"

#include <keel/sockaddr.h>
#include "../../src/resolve_sync.h"   /* kl_resolve_sync decl */

#include <stdint.h>
#include <stddef.h>

/* Parse "a.b.c.d" (each octet 0..255) into 4 bytes. -1 on any non-numeric input. */
static int parse_ipv4(const char *host, uint8_t out[4]) {
    int oct = 0, val = -1;
    const char *p = host;
    for (;;) {
        if (*p >= '0' && *p <= '9') {
            if (val < 0) val = 0;
            val = val * 10 + (*p - '0');
            if (val > 255) return -1;
        } else if (*p == '.' || *p == '\0') {
            if (val < 0 || oct > 3) return -1;
            out[oct++] = (uint8_t)val;
            val = -1;
            if (*p == '\0') break;
        } else {
            return -1;   /* non-numeric host — this seam is numeric-only; hostnames resolve via the async cfg.resolver */
        }
        p++;
    }
    return oct == 4 ? 0 : -1;
}

#ifdef KL_U4_STATIC_HOST
/* Freestanding string compare (no libc guaranteed in this TU). */
static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
#endif

int kl_resolve_sync(const char *host, uint16_t port, int socktype,
                    KlSockAddr *out, int max, int *n) {
    (void)socktype;
    if (!host || !out || max <= 0 || !n) return -1;
    uint8_t ip[4];
#ifdef KL_U4_STATIC_HOST
    /* Optional compile-time single-entry /etc/hosts: a name→IPv4 mapping so a client
     * with no async resolver wired can still dial a HOSTNAME — needed so TLS verifies the
     * server cert against a dNSName SAN (production TLS) rather than a bare IP. Set via
     * -DKL_U4_STATIC_HOST="name" -DKL_U4_STATIC_IP="a.b.c.d". */
    if (streq(host, KL_U4_STATIC_HOST)) {
        if (parse_ipv4(KL_U4_STATIC_IP, ip) != 0) return -1;
        if (kl_sockaddr_from_ipv4(&out[0], ip, port) != 0) return -1;
        *n = 1;
        return 0;
    }
#endif
    if (parse_ipv4(host, ip) == 0) {
        if (kl_sockaddr_from_ipv4(&out[0], ip, port) != 0) return -1;
        *n = 1;
        return 0;
    }

    /* Non-numeric host: this seam resolves numeric literals only. DNS is an ASYNC protocol
     * consumer, not a sync platform-seam capability — a freestanding client that needs a
     * hostname injects cfg.resolver (a stock kl_dns_resolver_create over the EFI_UDP4 provider;
     * see 6.4c dgram_dns_selftest.c). Fail closed here. */
    return -1;
}
