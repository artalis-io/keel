#include "utest.h"
#include <keel/proxy_protocol.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>

/* ── CIDR ────────────────────────────────────────────────────────────── */

static struct sockaddr_in v4(const char *ip) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}
static struct sockaddr_in6 v6(const char *ip) {
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    inet_pton(AF_INET6, ip, &a.sin6_addr);
    return a;
}

UTEST(cidr, parse_and_match_v4) {
    KlCidr list[8];
    int n = kl_cidr_parse_list("10.0.0.0/8, 192.168.1.0/24, 127.0.0.1/32", list, 8);
    ASSERT_EQ(3, n);

    struct sockaddr_in a;
    a = v4("10.9.8.7");     ASSERT_EQ(1, kl_cidr_match(list, n, (struct sockaddr *)&a));
    a = v4("192.168.1.50"); ASSERT_EQ(1, kl_cidr_match(list, n, (struct sockaddr *)&a));
    a = v4("192.168.2.50"); ASSERT_EQ(0, kl_cidr_match(list, n, (struct sockaddr *)&a));
    a = v4("127.0.0.1");    ASSERT_EQ(1, kl_cidr_match(list, n, (struct sockaddr *)&a));
    a = v4("127.0.0.2");    ASSERT_EQ(0, kl_cidr_match(list, n, (struct sockaddr *)&a));
    a = v4("8.8.8.8");      ASSERT_EQ(0, kl_cidr_match(list, n, (struct sockaddr *)&a));
}

UTEST(cidr, parse_and_match_v6) {
    KlCidr list[4];
    int n = kl_cidr_parse_list("::1/128,fd00::/8", list, 4);
    ASSERT_EQ(2, n);
    struct sockaddr_in6 a;
    a = v6("::1");        ASSERT_EQ(1, kl_cidr_match(list, n, (struct sockaddr *)&a));
    a = v6("::2");        ASSERT_EQ(0, kl_cidr_match(list, n, (struct sockaddr *)&a));
    a = v6("fd12::abcd"); ASSERT_EQ(1, kl_cidr_match(list, n, (struct sockaddr *)&a));
    /* family mismatch: a v4 addr never matches a v6 CIDR. */
    struct sockaddr_in a4 = v4("127.0.0.1");
    ASSERT_EQ(0, kl_cidr_match(list, n, (struct sockaddr *)&a4));
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
    struct sockaddr_storage peer;
    socklen_t plen = 0;
    size_t consumed = 0;
    KlProxyResult r = kl_proxy_parse((const uint8_t *)h, strlen(h), &consumed,
                                     &peer, &plen);
    ASSERT_EQ(KL_PROXY_OK, r);
    ASSERT_EQ((size_t)43, consumed);   /* 41-byte line + CRLF */
    ASSERT_EQ((socklen_t)sizeof(struct sockaddr_in), plen);
    struct sockaddr_in *s4 = (struct sockaddr_in *)&peer;
    char ip[64];
    inet_ntop(AF_INET, &s4->sin_addr, ip, sizeof(ip));
    ASSERT_STREQ("203.0.113.7", ip);
    ASSERT_EQ(56324, ntohs(s4->sin_port));
}

UTEST(proxy_v1, tcp6_and_unknown) {
    const char *h6 = "PROXY TCP6 2001:db8::1 2001:db8::2 5000 443\r\n";
    struct sockaddr_storage peer; socklen_t plen = 0; size_t consumed = 0;
    ASSERT_EQ(KL_PROXY_OK, kl_proxy_parse((const uint8_t *)h6, strlen(h6),
                                          &consumed, &peer, &plen));
    ASSERT_EQ((socklen_t)sizeof(struct sockaddr_in6), plen);

    const char *hu = "PROXY UNKNOWN\r\n";
    ASSERT_EQ(KL_PROXY_OK, kl_proxy_parse((const uint8_t *)hu, strlen(hu),
                                          &consumed, &peer, &plen));
    ASSERT_EQ((socklen_t)0, plen);   /* keep socket addr */
}

UTEST(proxy_v1, partial_and_invalid_and_none) {
    struct sockaddr_storage peer; socklen_t plen; size_t consumed;
    /* prefix of "PROXY " → need more */
    ASSERT_EQ(KL_PROXY_NEED_MORE, kl_proxy_parse((const uint8_t *)"PRO", 3,
                                                 &consumed, &peer, &plen));
    /* looks like v1 but no CRLF yet */
    const char *partial = "PROXY TCP4 1.2.3.4 5.6.7.8 80 90";
    ASSERT_EQ(KL_PROXY_NEED_MORE, kl_proxy_parse((const uint8_t *)partial,
                                                 strlen(partial), &consumed, &peer, &plen));
    /* bad proto */
    const char *bad = "PROXY TCPX 1.2.3.4 5.6.7.8 80 90\r\n";
    ASSERT_EQ(KL_PROXY_INVALID, kl_proxy_parse((const uint8_t *)bad, strlen(bad),
                                               &consumed, &peer, &plen));
    /* not a proxy header at all (HTTP) */
    const char *http = "GET / HTTP/1.1\r\n";
    ASSERT_EQ(KL_PROXY_NONE, kl_proxy_parse((const uint8_t *)http, strlen(http),
                                            &consumed, &peer, &plen));
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
    struct sockaddr_storage peer; socklen_t plen = 0; size_t consumed = 0;
    ASSERT_EQ(KL_PROXY_OK, kl_proxy_parse(h, hlen, &consumed, &peer, &plen));
    ASSERT_EQ((size_t)28, consumed);
    struct sockaddr_in *s4 = (struct sockaddr_in *)&peer;
    char ip[64]; inet_ntop(AF_INET, &s4->sin_addr, ip, sizeof(ip));
    ASSERT_STREQ("198.51.100.9", ip);
    ASSERT_EQ(40000, ntohs(s4->sin_port));
}

UTEST(proxy_v2, local_and_partial) {
    uint8_t h[64];
    size_t hlen = build_v2_inet(h, 0x0, "1.2.3.4", 1);   /* LOCAL command */
    struct sockaddr_storage peer; socklen_t plen = 99; size_t consumed = 0;
    ASSERT_EQ(KL_PROXY_OK, kl_proxy_parse(h, hlen, &consumed, &peer, &plen));
    ASSERT_EQ((socklen_t)0, plen);   /* LOCAL → keep socket addr */

    /* Only the signature so far → need more. */
    ASSERT_EQ(KL_PROXY_NEED_MORE, kl_proxy_parse(h, 12, &consumed, &peer, &plen));
    ASSERT_EQ(KL_PROXY_NEED_MORE, kl_proxy_parse(h, 20, &consumed, &peer, &plen));

    /* Bad version. */
    uint8_t bad[28]; memcpy(bad, h, 28); bad[12] = 0x31;  /* version 3 */
    ASSERT_EQ(KL_PROXY_INVALID, kl_proxy_parse(bad, 28, &consumed, &peer, &plen));
}

UTEST_MAIN();
