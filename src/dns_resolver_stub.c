/*
 * dns_resolver_stub.c — Windows placeholder for the built-in async DNS resolver.
 *
 * The real dns_resolver.c is built on the UDP module (kl_udp_*), which isn't
 * ported to Winsock yet, so it's excluded from the Windows build. This stub
 * provides the one public symbol the client references (kl_dns_resolver_create)
 * so the TCP core links. Returning NULL makes the async client fall back to
 * blocking getaddrinfo — the same documented path as cfg->system_dns and
 * auto-create failure (see get_resolver in client.c). Users can still supply
 * their own KlResolver via KlClientConfig.resolver.
 *
 * Replace this by porting udp.c + dns_resolver.c to Winsock.
 */

#include <keel/dns_resolver.h>

KlResolver *kl_dns_resolver_create(KlEventCtx *ctx, const KlDnsResolverConfig *cfg) {
    (void)ctx;
    (void)cfg;
    return NULL;
}
