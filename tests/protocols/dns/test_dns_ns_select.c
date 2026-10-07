/*
 * Nameserver-list selection for the built-in DNS resolver: which of the discovered nameserver
 * strings become the resolver's server list (one socket, so one family). A pure function over
 * strings, so it runs on every platform (test_dns_resolver needs a POSIX mock nameserver).
 */
#include "utest.h"
#include <keel/sockaddr.h>
#include <stdint.h>

/* Internal test hook (not in the public header): the pure nameserver-list selection behind
 * kl_dns_resolver_create. Fills out[] with up to max usable, de-duplicated servers of one family and
 * returns how many (0 = none usable); *family receives that family. */
extern int kl_dns_select_ns(const char *const *ns, int count, uint16_t defport,
                            KlSockAddr *out, int max, KlAddrFamily *family);

static int ns_is(const KlSockAddr *a, const char *ip, uint16_t port) {
    KlSockAddr b;
    return kl_sockaddr_parse(&b, ip, port) == 0 && kl_sockaddr_equal(a, &b);
}

/* Windows lists a router-advertised fe80:: resolver first (without its scope id). It cannot be
 * reached (a link-local address needs its interface), and it must not lock the list to IPv6 while an
 * IPv4 server is available. */
UTEST(dns_ns, link_local_first_does_not_lock_out_ipv4) {
    const char *list[] = { "fe80::1", "192.0.2.53" };
    KlSockAddr out[3];
    KlAddrFamily fam = KL_AF_UNSPEC;
    int n = kl_dns_select_ns(list, 2, 53, out, 3, &fam);
    ASSERT_EQ(1, n);                         /* was: 1, the fe80::1 entry alone */
    ASSERT_EQ(KL_AF_INET, fam);              /* was: KL_AF_INET6 */
    ASSERT_TRUE(ns_is(&out[0], "192.0.2.53", 53));
}

UTEST(dns_ns, first_usable_entry_picks_the_family) {
    const char *list[] = { "fe80::1", "2001:db8::53", "192.0.2.53" };
    KlSockAddr out[3];
    KlAddrFamily fam = KL_AF_UNSPEC;
    int n = kl_dns_select_ns(list, 3, 53, out, 3, &fam);
    ASSERT_EQ(1, n);
    ASSERT_EQ(KL_AF_INET6, fam);
    ASSERT_TRUE(ns_is(&out[0], "2001:db8::53", 53));
}

UTEST(dns_ns, one_family_deduplicated) {
    const char *list[] = { "192.0.2.53", "2001:db8::53", "192.0.2.53", "192.0.2.54#5353" };
    KlSockAddr out[3];
    KlAddrFamily fam = KL_AF_UNSPEC;
    int n = kl_dns_select_ns(list, 4, 53, out, 3, &fam);
    ASSERT_EQ(2, n);
    ASSERT_EQ(KL_AF_INET, fam);
    ASSERT_TRUE(ns_is(&out[0], "192.0.2.53", 53));
    ASSERT_TRUE(ns_is(&out[1], "192.0.2.54", 5353));
}

UTEST(dns_ns, only_link_local_is_none_usable) {
    const char *list[] = { "fe80::1", "febf::2", "not-an-ip" };
    KlSockAddr out[3];
    KlAddrFamily fam = KL_AF_UNSPEC;
    ASSERT_EQ(0, kl_dns_select_ns(list, 3, 53, out, 3, &fam));
}

UTEST_MAIN()
