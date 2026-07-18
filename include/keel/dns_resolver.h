#ifndef KEEL_DNS_RESOLVER_H
#define KEEL_DNS_RESOLVER_H

#include <keel/allocator.h>
#include <keel/event_ctx.h>
#include <keel/resolver.h>

#include <stddef.h>
#include <stdint.h>

/**
 * dns_resolver.h — Built-in async DNS resolver over KlUdp.
 *
 * Implements the KlResolver vtable using non-blocking UDP DNS queries, so it
 * drops into KlClientConfig.resolver and replaces the blocking getaddrinfo
 * fallback. Queries A then AAAA (or the reverse with prefer_ipv6), retransmits
 * on timeout, and chases the first matching address record in the response.
 *
 * v1 scope: recursive resolution via a configured nameserver (no TCP fallback
 * on truncation, no EDNS0, no DNSSEC, no /etc/hosts or search domains).
 */

/**
 * @brief DNS resolver configuration.
 */
typedef struct {
    const char  *nameserver;  /**< Numeric NS address; NULL = /etc/resolv.conf, else 127.0.0.1. */
    uint16_t     port;        /**< NS port; 0 = 53. */
    int          timeout_ms;  /**< Per-attempt timeout; 0 = 5000. */
    int          attempts;    /**< Transmits per family before failing; 0 = 2. */
    int          prefer_ipv6; /**< Query AAAA before A. */
    KlAllocator *alloc;       /**< NULL = event-context allocator. */
} KlDnsResolverConfig;

/**
 * @brief Create an async DNS resolver.
 *
 * @param ctx Event context (borrowed — must outlive the resolver).
 * @param cfg Configuration (may be NULL for all defaults).
 * @return A KlResolver* to plug into KlClientConfig.resolver, or NULL on error.
 *         Free via the vtable's destroy().
 */
KlResolver *kl_dns_resolver_create(KlEventCtx *ctx, const KlDnsResolverConfig *cfg);

/** DNS record types (subset used by the resolver). */
#define KL_DNS_TYPE_A     1
#define KL_DNS_TYPE_AAAA  28

/**
 * @brief Parse a DNS response and extract the first matching address record.
 *
 * Exposed for unit tests and the fuzz target. Bounds-safe against malformed,
 * truncated, or hostile packets (every read is checked against @p len, and
 * name-label traversal is capped). Does not set the address port.
 *
 * @param pkt        Response bytes.
 * @param len        Response length.
 * @param expect_id  Transaction ID the response must carry.
 * @param want_qtype KL_DNS_TYPE_A or KL_DNS_TYPE_AAAA.
 * @param out        Filled with the address on success (addr/addrlen/ai_family).
 * @return 0 if a matching record was found and @p out filled, -1 otherwise.
 */
int kl_dns_parse_response(const uint8_t *pkt, size_t len, uint16_t expect_id,
                          int want_qtype, KlResolveResult *out);

#endif /* KEEL_DNS_RESOLVER_H */
