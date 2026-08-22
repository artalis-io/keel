#include "utest.h"
#include <keel/proxy_protocol.h>
#include <string.h>
#include "net_compat.h"

/* ── CIDR ────────────────────────────────────────────────────────────── */

static KlSockAddr v4(const char *ip) {
    uint8_t b[4];
    inet_pton(AF_INET, ip, b);
    KlSockAddr a;
    kl_sockaddr_from_ipv4(&a, b, 0);
    return a;
}
static KlSockAddr v6(const char *ip) {
    uint8_t b[16];
    inet_pton(AF_INET6, ip, b);
    KlSockAddr a;
    kl_sockaddr_from_ipv6(&a, b, 0, 0);
    return a;
}

UTEST(cidr, parse_and_match_v4) {
    KlCidr list[8];
    int n = kl_cidr_parse_list("10.0.0.0/8, 192.168.1.0/24, 127.0.0.1/32", list, 8);
    ASSERT_EQ(3, n);

    KlSockAddr a;
    a = v4("10.9.8.7");     ASSERT_EQ(1, kl_cidr_match(list, n, &a));
    a = v4("192.168.1.50"); ASSERT_EQ(1, kl_cidr_match(list, n, &a));
    a = v4("192.168.2.50"); ASSERT_EQ(0, kl_cidr_match(list, n, &a));
    a = v4("127.0.0.1");    ASSERT_EQ(1, kl_cidr_match(list, n, &a));
    a = v4("127.0.0.2");    ASSERT_EQ(0, kl_cidr_match(list, n, &a));
    a = v4("8.8.8.8");      ASSERT_EQ(0, kl_cidr_match(list, n, &a));
}

UTEST(cidr, parse_and_match_v6) {
    KlCidr list[4];
    int n = kl_cidr_parse_list("::1/128,fd00::/8", list, 4);
    ASSERT_EQ(2, n);
    KlSockAddr a;
    a = v6("::1");        ASSERT_EQ(1, kl_cidr_match(list, n, &a));
    a = v6("::2");        ASSERT_EQ(0, kl_cidr_match(list, n, &a));
    a = v6("fd12::abcd"); ASSERT_EQ(1, kl_cidr_match(list, n, &a));
    /* family mismatch: a v4 addr never matches a v6 CIDR. */
    KlSockAddr a4 = v4("127.0.0.1");
    ASSERT_EQ(0, kl_cidr_match(list, n, &a4));
}

UTEST(cidr, malformed) {
    KlCidr list[4];
    ASSERT_EQ(-1, kl_cidr_parse_list("10.0.0.0/33", list, 4));    /* bits too big */
    ASSERT_EQ(-1, kl_cidr_parse_list("not-an-ip", list, 4));
    ASSERT_EQ(-1, kl_cidr_parse_list("10.0.0.0/x", list, 4));
    ASSERT_EQ(0,  kl_cidr_parse_list("", list, 4));
    ASSERT_EQ(0,  kl_cidr_parse_list(NULL, list, 4));
}

/* ── PROXY v1 ────────────────────────────────────────────────────────── */

UTEST(proxy_v1, tcp4) {
    const char *h = "PROXY TCP4 203.0.113.7 10.0.0.1 56324 443\r\nGET / ...";
    KlSockAddr peer;
    size_t consumed = 0;
    KlProxyResult r = kl_proxy_parse((const uint8_t *)h, strlen(h), &consumed, &peer);
    ASSERT_EQ(KL_PROXY_OK, r);
    ASSERT_EQ((size_t)43, consumed);   /* 41-byte line + CRLF */
    ASSERT_EQ((int)KL_AF_INET, (int)kl_sockaddr_family(&peer));
    char ip[64];
    inet_ntop(AF_INET, peer.u.ip, ip, sizeof(ip));
    ASSERT_STREQ("203.0.113.7", ip);
    ASSERT_EQ(56324, (int)kl_sockaddr_port(&peer));
}

UTEST(proxy_v1, tcp6_and_unknown) {
    const char *h6 = "PROXY TCP6 2001:db8::1 2001:db8::2 5000 443\r\n";
    KlSockAddr peer; size_t consumed = 0;
    ASSERT_EQ(KL_PROXY_OK, kl_proxy_parse((const uint8_t *)h6, strlen(h6),
                                          &consumed, &peer));
    ASSERT_EQ((int)KL_AF_INET6, (int)kl_sockaddr_family(&peer));

    const char *hu = "PROXY UNKNOWN\r\n";
    ASSERT_EQ(KL_PROXY_OK, kl_proxy_parse((const uint8_t *)hu, strlen(hu),
                                          &consumed, &peer));
    ASSERT_EQ((int)KL_AF_UNSPEC, (int)kl_sockaddr_family(&peer));   /* keep socket addr */
}

UTEST(proxy_v1, partial_and_invalid_and_none) {
    KlSockAddr peer; size_t consumed;
    /* prefix of "PROXY " → need more */
    ASSERT_EQ(KL_PROXY_NEED_MORE, kl_proxy_parse((const uint8_t *)"PRO", 3,
                                                 &consumed, &peer));
    /* looks like v1 but no CRLF yet */
    const char *partial = "PROXY TCP4 1.2.3.4 5.6.7.8 80 90";
    ASSERT_EQ(KL_PROXY_NEED_MORE, kl_proxy_parse((const uint8_t *)partial,
                                                 strlen(partial), &consumed, &peer));
    /* bad proto */
    const char *bad = "PROXY TCPX 1.2.3.4 5.6.7.8 80 90\r\n";
    ASSERT_EQ(KL_PROXY_INVALID, kl_proxy_parse((const uint8_t *)bad, strlen(bad),
                                               &consumed, &peer));
    /* not a proxy header at all (HTTP) */
    const char *http = "GET / HTTP/1.1\r\n";
    ASSERT_EQ(KL_PROXY_NONE, kl_proxy_parse((const uint8_t *)http, strlen(http),
                                            &consumed, &peer));
}

/* ── PROXY v2 ────────────────────────────────────────────────────────── */

static size_t build_v2_inet(uint8_t *out, uint8_t cmd, const char *src_ip,
                            uint16_t src_port) {
    static const uint8_t sig[12] = {0x0D,0x0A,0x0D,0x0A,0x00,0x0D,0x0A,0x51,0x55,0x49,0x54,0x0A};
    memcpy(out, sig, 12);
    out[12] = (uint8_t)(0x20 | cmd);   /* v2 | cmd */
    out[13] = 0x11;                     /* AF_INET | STREAM */
    out[14] = 0x00; out[15] = 0x0C;     /* block len = 12 */
    struct in_addr s, d; memset(&d, 0, sizeof(d));
    inet_pton(AF_INET, src_ip, &s);
    memcpy(out + 16, &s, 4);            /* src */
    memcpy(out + 20, &d, 4);            /* dst */
    uint16_t sp = htons(src_port), dp = htons(443);
    memcpy(out + 24, &sp, 2);
    memcpy(out + 26, &dp, 2);
    return 28;
}

UTEST(proxy_v2, inet_proxy) {
    uint8_t h[64];
    size_t hlen = build_v2_inet(h, 0x1, "198.51.100.9", 40000);
    KlSockAddr peer; size_t consumed = 0;
    ASSERT_EQ(KL_PROXY_OK, kl_proxy_parse(h, hlen, &consumed, &peer));
    ASSERT_EQ((size_t)28, consumed);
    ASSERT_EQ((int)KL_AF_INET, (int)kl_sockaddr_family(&peer));
    char ip[64]; inet_ntop(AF_INET, peer.u.ip, ip, sizeof(ip));
    ASSERT_STREQ("198.51.100.9", ip);
    ASSERT_EQ(40000, (int)kl_sockaddr_port(&peer));
}

UTEST(proxy_v2, local_and_partial) {
    uint8_t h[64];
    size_t hlen = build_v2_inet(h, 0x0, "1.2.3.4", 1);   /* LOCAL command */
    KlSockAddr peer; size_t consumed = 0;
    ASSERT_EQ(KL_PROXY_OK, kl_proxy_parse(h, hlen, &consumed, &peer));
    ASSERT_EQ((int)KL_AF_UNSPEC, (int)kl_sockaddr_family(&peer));   /* LOCAL → keep socket addr */

    /* Only the signature so far → need more. */
    ASSERT_EQ(KL_PROXY_NEED_MORE, kl_proxy_parse(h, 12, &consumed, &peer));
    ASSERT_EQ(KL_PROXY_NEED_MORE, kl_proxy_parse(h, 20, &consumed, &peer));

    /* Bad version. */
    uint8_t bad[28]; memcpy(bad, h, 28); bad[12] = 0x31;  /* version 3 */
    ASSERT_EQ(KL_PROXY_INVALID, kl_proxy_parse(bad, 28, &consumed, &peer));
}

UTEST_MAIN();
