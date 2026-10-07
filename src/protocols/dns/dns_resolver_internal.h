/*
 * dns_resolver_internal.h: INTERNAL. The built-in resolver's provider-explicit creator.
 *
 * kl_dns_resolver_create takes its socket provider from the event context (ctx->sockets). A consumer
 * that owns a provider of its own (the async HTTP client, whose provider is per client and never
 * written into the shared ctx) creates its resolver with that provider instead, so the resolver's
 * UDP socket and its TCP fallback live in the same handle domain as the consumer's connections.
 *
 * INTERNAL header: not installed, no ABI commitment.
 */
#ifndef KEEL_SRC_DNS_RESOLVER_INTERNAL_H
#define KEEL_SRC_DNS_RESOLVER_INTERNAL_H

#include <keel/dns_resolver.h>   /* KlDnsResolverConfig, KlResolver, KlEventCtx */
#include <keel/socket.h>         /* KlSocketProvider */

/* As kl_dns_resolver_create, with every socket made through `sp` (NULL = the built-in default)
 * rather than ctx->sockets. The provider must outlive the resolver. */
KlResolver *kl_dns_resolver_create_sp(KlEventCtx *ctx, const KlSocketProvider *sp,
                                      const KlDnsResolverConfig *cfg);

#endif /* KEEL_SRC_DNS_RESOLVER_INTERNAL_H */
