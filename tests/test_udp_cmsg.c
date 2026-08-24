/*
 * test_udp_cmsg.c: the shared POSIX UDP send-cmsg builder (udp_cmsg.h): overflow-safe capacity
 * checks, fail-on-requested-but-unbuilt, family resolution, and resulting-header level/type. Unit-
 * level (no event loop). Feature-specific cases are guarded by the same macros as the implementation,
 * with explicit requested-but-unavailable (-1) coverage on reduced-capability builds.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(__APPLE_USE_RFC_3542)
#define __APPLE_USE_RFC_3542
#endif

#include "utest.h"
#include "../src/udp_cmsg.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdalign.h>
#include <string.h>
#include <unistd.h>

static void mk_v4(struct sockaddr_in *a) {
    memset(a, 0, sizeof(*a)); a->sin_family = AF_INET; a->sin_addr.s_addr = htonl(0x7f000001);
}
static void mk_v6(struct sockaddr_in6 *a) {
    memset(a, 0, sizeof(*a)); a->sin6_family = AF_INET6; a->sin6_addr = in6addr_loopback;
}

/* Walk the built control buffer; return the number of cmsg records and fill level/type arrays
 * (up to `cap`). Uses a struct msghdr so CMSG_FIRSTHDR/NXTHDR do the bounds-correct walk. */
static int inspect(unsigned char *buf, size_t len, int *lvl, int *typ, int cap) {
    struct msghdr m; memset(&m, 0, sizeof(m));
    m.msg_control = buf; m.msg_controllen = len;
    int n = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
        if (n < cap) { lvl[n] = c->cmsg_level; typ[n] = c->cmsg_type; }
        n++;
    }
    return n;
}

/* nothing requested (src NULL, tos < 0) → success, out == 0. */
UTEST(udp_cmsg, nothing_requested_is_zero) {
    _Alignas(struct cmsghdr) unsigned char buf[64]; size_t out = 99;
    ASSERT_EQ(0, kl_udp_build_control(buf, sizeof(buf), NULL, -1, AF_INET, &out));
    ASSERT_EQ((size_t)0, out);
}

/* source-pin only (v4): exact-fit succeeds with an IPPROTO_IP/IP_PKTINFO header; one byte short fails.
 * On a build without IP_PKTINFO the request must fail (requested-but-unavailable). */
UTEST(udp_cmsg, source_v4) {
#if defined(IP_PKTINFO)
    struct sockaddr_in s; mk_v4(&s);
    size_t need = CMSG_SPACE(sizeof(struct in_pktinfo));
    _Alignas(struct cmsghdr) unsigned char buf[256]; size_t out;
    ASSERT_EQ(0, kl_udp_build_control(buf, need, (struct sockaddr *)&s, -1, AF_INET, &out));
    ASSERT_EQ(need, out);
    int lvl[2], typ[2];
    ASSERT_EQ(1, inspect(buf, out, lvl, typ, 2));
    ASSERT_EQ(IPPROTO_IP, lvl[0]); ASSERT_EQ(IP_PKTINFO, typ[0]);
    ASSERT_EQ(-1, kl_udp_build_control(buf, need - 1, (struct sockaddr *)&s, -1, AF_INET, &out));  /* short */
#else
    struct sockaddr_in s; mk_v4(&s);
    _Alignas(struct cmsghdr) unsigned char buf[256]; size_t out;
    ASSERT_EQ(-1, kl_udp_build_control(buf, sizeof(buf), (struct sockaddr *)&s, -1, AF_INET, &out));
#endif
}

/* source-pin only (v6): exact-fit yields IPPROTO_IPV6/IPV6_PKTINFO; one short fails; else -1. */
UTEST(udp_cmsg, source_v6) {
#if defined(IPV6_PKTINFO)
    struct sockaddr_in6 s; mk_v6(&s);
    size_t need = CMSG_SPACE(sizeof(struct in6_pktinfo));
    _Alignas(struct cmsghdr) unsigned char buf[256]; size_t out;
    ASSERT_EQ(0, kl_udp_build_control(buf, need, (struct sockaddr *)&s, -1, AF_INET6, &out));
    ASSERT_EQ(need, out);
    int lvl[2], typ[2];
    ASSERT_EQ(1, inspect(buf, out, lvl, typ, 2));
    ASSERT_EQ(IPPROTO_IPV6, lvl[0]); ASSERT_EQ(IPV6_PKTINFO, typ[0]);
    ASSERT_EQ(-1, kl_udp_build_control(buf, need - 1, (struct sockaddr *)&s, -1, AF_INET6, &out));
#else
    struct sockaddr_in6 s; mk_v6(&s);
    _Alignas(struct cmsghdr) unsigned char buf[256]; size_t out;
    ASSERT_EQ(-1, kl_udp_build_control(buf, sizeof(buf), (struct sockaddr *)&s, -1, AF_INET6, &out));
#endif
}

/* TOS only, v4: exact-fit yields IPPROTO_IP/IP_TOS; one short fails; else -1. */
UTEST(udp_cmsg, tos_v4) {
#if defined(IP_TOS)
    size_t need = CMSG_SPACE(sizeof(int));
    _Alignas(struct cmsghdr) unsigned char buf[256]; size_t out;
    ASSERT_EQ(0, kl_udp_build_control(buf, need, NULL, 0x28, AF_INET, &out));
    ASSERT_EQ(need, out);
    int lvl[2], typ[2];
    ASSERT_EQ(1, inspect(buf, out, lvl, typ, 2));
    ASSERT_EQ(IPPROTO_IP, lvl[0]); ASSERT_EQ(IP_TOS, typ[0]);
    ASSERT_EQ(-1, kl_udp_build_control(buf, need - 1, NULL, 0x28, AF_INET, &out));
#else
    _Alignas(struct cmsghdr) unsigned char buf[256]; size_t out;
    ASSERT_EQ(-1, kl_udp_build_control(buf, sizeof(buf), NULL, 0x28, AF_INET, &out));
#endif
}

/* TOS only, v6 (connected IPv6 path): exact-fit yields IPPROTO_IPV6/IPV6_TCLASS (never IP_TOS). */
UTEST(udp_cmsg, tos_v6_is_tclass) {
#if defined(IPV6_TCLASS)
    size_t need = CMSG_SPACE(sizeof(int));
    _Alignas(struct cmsghdr) unsigned char buf[256]; size_t out;
    ASSERT_EQ(0, kl_udp_build_control(buf, need, NULL, 0x28, AF_INET6, &out));
    ASSERT_EQ(need, out);
    int lvl[2], typ[2];
    ASSERT_EQ(1, inspect(buf, out, lvl, typ, 2));
    ASSERT_EQ(IPPROTO_IPV6, lvl[0]); ASSERT_EQ(IPV6_TCLASS, typ[0]);
    ASSERT_EQ(-1, kl_udp_build_control(buf, need - 1, NULL, 0x28, AF_INET6, &out));
#else
    _Alignas(struct cmsghdr) unsigned char buf[256]; size_t out;
    ASSERT_EQ(-1, kl_udp_build_control(buf, sizeof(buf), NULL, 0x28, AF_INET6, &out));
#endif
}

/* combined source-pin + TOS (v4): two records: IP_PKTINFO then IP_TOS; exact-fit, one short fails. */
UTEST(udp_cmsg, combined_v4) {
#if defined(IP_PKTINFO) && defined(IP_TOS)
    struct sockaddr_in s; mk_v4(&s);
    size_t need = CMSG_SPACE(sizeof(struct in_pktinfo)) + CMSG_SPACE(sizeof(int));
    _Alignas(struct cmsghdr) unsigned char buf[256]; size_t out;
    ASSERT_EQ(0, kl_udp_build_control(buf, need, (struct sockaddr *)&s, 0x10, AF_INET, &out));
    ASSERT_EQ(need, out);
    int lvl[2], typ[2];
    ASSERT_EQ(2, inspect(buf, out, lvl, typ, 2));
    ASSERT_EQ(IPPROTO_IP, lvl[0]); ASSERT_EQ(IP_PKTINFO, typ[0]);
    ASSERT_EQ(IPPROTO_IP, lvl[1]); ASSERT_EQ(IP_TOS,     typ[1]);
    ASSERT_EQ(-1, kl_udp_build_control(buf, need - 1, (struct sockaddr *)&s, 0x10, AF_INET, &out));
#endif
}

/* a requested cmsg with an unknown family cannot be built → -1 (never a silent no-control send). */
UTEST(udp_cmsg, unknown_family_fails) {
    _Alignas(struct cmsghdr) unsigned char buf[256]; size_t out;
    ASSERT_EQ(-1, kl_udp_build_control(buf, sizeof(buf), NULL, 0x10, AF_UNSPEC, &out));   /* TOS, no family */
}

/* family resolution: dest, else src, else getsockname; never defaults to AF_INET on a v6 fd. */
UTEST(udp_cmsg, send_family_resolution) {
    struct sockaddr_in d4;  mk_v4(&d4);
    struct sockaddr_in6 s6; mk_v6(&s6);
    ASSERT_EQ(AF_INET,  kl_udp_send_family(-1, (struct sockaddr *)&d4, NULL));          /* from dest */
    ASSERT_EQ(AF_INET6, kl_udp_send_family(-1, NULL, (struct sockaddr *)&s6));          /* from src  */
    /* connected (no dest/src) → getsockname on the fd's own family, never AF_INET default. */
    int fd6 = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd6 >= 0) { ASSERT_EQ(AF_INET6, kl_udp_send_family(fd6, NULL, NULL)); close(fd6); }
    int fd4 = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd4 >= 0) { ASSERT_EQ(AF_INET, kl_udp_send_family(fd4, NULL, NULL)); close(fd4); }
}

UTEST_MAIN();
