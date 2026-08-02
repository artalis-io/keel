#include "utest.h"
#include <keel/sockaddr.h>
#include <string.h>

/* ── construct-from-wire + accessors ──────────────────────────────────────── */

UTEST(sockaddr, from_ipv4) {
    KlSockAddr a;
    uint8_t ip[4] = { 192, 168, 1, 5 };
    ASSERT_EQ(0, kl_sockaddr_from_ipv4(&a, ip, 8080));
    ASSERT_EQ((int)KL_AF_INET, (int)kl_sockaddr_family(&a));
    ASSERT_EQ(8080, (int)kl_sockaddr_port(&a));
    ASSERT_EQ(4, (int)a.addr_len);
    ASSERT_EQ(0, memcmp(a.u.ip, ip, 4));
}

UTEST(sockaddr, from_ipv6) {
    KlSockAddr a;
    uint8_t ip[16] = { 0x20,0x01,0x0d,0xb8, 0,0,0,0, 0,0,0,0, 0,0,0,1 };
    ASSERT_EQ(0, kl_sockaddr_from_ipv6(&a, ip, 443, 3));
    ASSERT_EQ((int)KL_AF_INET6, (int)kl_sockaddr_family(&a));
    ASSERT_EQ(443, (int)kl_sockaddr_port(&a));
    ASSERT_EQ(16, (int)a.addr_len);
    ASSERT_EQ(3u, a.scope_id);
    ASSERT_EQ(0, memcmp(a.u.ip, ip, 16));
}

UTEST(sockaddr, from_unix) {
    KlSockAddr a;
    ASSERT_EQ(0, kl_sockaddr_from_unix(&a, "/tmp/keel.sock"));
    ASSERT_EQ((int)KL_AF_UNIX, (int)kl_sockaddr_family(&a));
    ASSERT_EQ((int)strlen("/tmp/keel.sock"), (int)a.addr_len);
    ASSERT_STREQ("/tmp/keel.sock", a.u.path);
}

UTEST(sockaddr, from_unix_rejects_empty_and_toolong) {
    KlSockAddr a;
    char big[KL_UNIX_PATH_MAX + 8];
    memset(big, 'x', sizeof big);
    big[sizeof big - 1] = '\0';
    ASSERT_EQ(-1, kl_sockaddr_from_unix(&a, ""));
    ASSERT_EQ(-1, kl_sockaddr_from_unix(&a, big));
}

UTEST(sockaddr, null_guards) {
    KlSockAddr a;
    uint8_t ip[4] = { 1, 2, 3, 4 };
    ASSERT_EQ(-1, kl_sockaddr_from_ipv4(NULL, ip, 80));
    ASSERT_EQ(-1, kl_sockaddr_from_ipv4(&a, NULL, 80));
    ASSERT_EQ((int)KL_AF_UNSPEC, (int)kl_sockaddr_family(NULL));
    ASSERT_EQ(0, (int)kl_sockaddr_port(NULL));
}

UTEST(sockaddr, set_port) {
    KlSockAddr a;
    uint8_t ip[4] = { 10, 0, 0, 1 };
    kl_sockaddr_from_ipv4(&a, ip, 80);
    kl_sockaddr_set_port(&a, 9090);
    ASSERT_EQ(9090, (int)kl_sockaddr_port(&a));
}

/* ── comparison ───────────────────────────────────────────────────────────── */

UTEST(sockaddr, equal_and_equal_addr) {
    KlSockAddr a, b;
    uint8_t ip[4] = { 127, 0, 0, 1 };
    kl_sockaddr_from_ipv4(&a, ip, 80);
    kl_sockaddr_from_ipv4(&b, ip, 80);
    ASSERT_TRUE(kl_sockaddr_equal(&a, &b));
    ASSERT_TRUE(kl_sockaddr_equal_addr(&a, &b));

    kl_sockaddr_set_port(&b, 81);
    ASSERT_FALSE(kl_sockaddr_equal(&a, &b));       /* port differs */
    ASSERT_TRUE(kl_sockaddr_equal_addr(&a, &b));   /* host same */

    uint8_t ip2[4] = { 127, 0, 0, 2 };
    kl_sockaddr_from_ipv4(&b, ip2, 80);
    ASSERT_FALSE(kl_sockaddr_equal_addr(&a, &b));
}

UTEST(sockaddr, equal_cross_family) {
    KlSockAddr v4, v6;
    uint8_t ip4[4] = { 0, 0, 0, 0 };
    uint8_t ip6[16] = { 0 };
    kl_sockaddr_from_ipv4(&v4, ip4, 0);
    kl_sockaddr_from_ipv6(&v6, ip6, 0, 0);
    ASSERT_FALSE(kl_sockaddr_equal(&v4, &v6));
}

UTEST(sockaddr, is_loopback) {
    KlSockAddr a;
    uint8_t v4lo[4] = { 127, 0, 0, 55 };
    kl_sockaddr_from_ipv4(&a, v4lo, 0);
    ASSERT_TRUE(kl_sockaddr_is_loopback(&a));

    uint8_t v4[4] = { 8, 8, 8, 8 };
    kl_sockaddr_from_ipv4(&a, v4, 0);
    ASSERT_FALSE(kl_sockaddr_is_loopback(&a));

    uint8_t v6lo[16] = { [15] = 1 };
    kl_sockaddr_from_ipv6(&a, v6lo, 0, 0);
    ASSERT_TRUE(kl_sockaddr_is_loopback(&a));

    uint8_t v6[16] = { 0x20, 0x01 };
    kl_sockaddr_from_ipv6(&a, v6, 0, 0);
    ASSERT_FALSE(kl_sockaddr_is_loopback(&a));
}

/* ── presentation ─────────────────────────────────────────────────────────── */

UTEST(sockaddr, format_ipv4) {
    KlSockAddr a;
    char buf[KL_SOCKADDR_STRLEN];
    uint8_t ip[4] = { 192, 168, 0, 1 };
    kl_sockaddr_from_ipv4(&a, ip, 8080);
    ASSERT_GT(kl_sockaddr_format(&a, buf, sizeof buf), 0);
    ASSERT_STREQ("192.168.0.1:8080", buf);
}

UTEST(sockaddr, format_ipv6_loopback) {
    KlSockAddr a;
    char buf[KL_SOCKADDR_STRLEN];
    uint8_t ip[16] = { [15] = 1 };
    kl_sockaddr_from_ipv6(&a, ip, 443, 0);
    ASSERT_GT(kl_sockaddr_format(&a, buf, sizeof buf), 0);
    ASSERT_STREQ("[::1]:443", buf);
}

UTEST(sockaddr, format_ipv6_compressed_midrun) {
    KlSockAddr a;
    char buf[KL_SOCKADDR_STRLEN];
    /* 2001:db8::1 */
    uint8_t ip[16] = { 0x20,0x01,0x0d,0xb8, 0,0,0,0, 0,0,0,0, 0,0,0,1 };
    kl_sockaddr_from_ipv6(&a, ip, 80, 0);
    ASSERT_GT(kl_sockaddr_format(&a, buf, sizeof buf), 0);
    ASSERT_STREQ("[2001:db8::1]:80", buf);
}

UTEST(sockaddr, format_ipv6_no_compression) {
    KlSockAddr a;
    char buf[KL_SOCKADDR_STRLEN];
    /* 1:2:3:4:5:6:7:8 — no zero run */
    uint8_t ip[16] = { 0,1, 0,2, 0,3, 0,4, 0,5, 0,6, 0,7, 0,8 };
    kl_sockaddr_from_ipv6(&a, ip, 0, 0);
    ASSERT_GT(kl_sockaddr_format(&a, buf, sizeof buf), 0);
    ASSERT_STREQ("[1:2:3:4:5:6:7:8]:0", buf);
}

UTEST(sockaddr, format_ipv6_trailing_zeros) {
    KlSockAddr a;
    char buf[KL_SOCKADDR_STRLEN];
    /* ff02:: */
    uint8_t ip[16] = { 0xff, 0x02 };
    kl_sockaddr_from_ipv6(&a, ip, 0, 0);
    ASSERT_GT(kl_sockaddr_format(&a, buf, sizeof buf), 0);
    ASSERT_STREQ("[ff02::]:0", buf);
}

UTEST(sockaddr, format_unix) {
    KlSockAddr a;
    char buf[KL_SOCKADDR_STRLEN];
    kl_sockaddr_from_unix(&a, "/run/keel.sock");
    ASSERT_GT(kl_sockaddr_format(&a, buf, sizeof buf), 0);
    ASSERT_STREQ("/run/keel.sock", buf);
}

UTEST(sockaddr, format_truncation) {
    KlSockAddr a;
    char buf[4];
    uint8_t ip[4] = { 192, 168, 0, 1 };
    kl_sockaddr_from_ipv4(&a, ip, 8080);
    ASSERT_EQ(-1, kl_sockaddr_format(&a, buf, sizeof buf));  /* doesn't fit */
}

UTEST(sockaddr, canonical_size) {
    /* stays a compact, fixed-layout value (<= the sockaddr_storage it replaces) */
    ASSERT_LE(sizeof(KlSockAddr), (size_t)128);
}

UTEST_MAIN();
