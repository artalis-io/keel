/*
 * test_datagram_live.c — the public KlDatagram over a REAL event loop + REAL loopback UDP (7B-4).
 *
 * The first LIVE backend binding: no mock. `kl_event_ctx_init` yields whatever the build's backend is —
 * a pollcomp COMPLETION loop under BACKEND=pollcomp (the 7B-4 target), a kqueue/epoll READINESS loop on
 * a default build — and the facade's §2.5 adapter builder selects the matching path. The caller prepares
 * the fd through the socket seam (KlDatagram contract), then does a real send→recv round-trip over
 * 127.0.0.1 and a clean close, so the live completion/readiness adapters are exercised end to end.
 *
 * sockets = NULL → the default POSIX provider; pollcomp drives overlapped ops on the ordinary fd, so no
 * provider plumbing is needed. The same test therefore runs green on both the completion and readiness
 * gates (the readiness §10 rows are only FLIPPED in 7B-6, but the adapter working here is a bonus).
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(__APPLE_USE_RFC_3542)
#define __APPLE_USE_RFC_3542
#endif

#include "../vendor/utest.h"

#include <keel/datagram.h>
#include <keel/datagram_detail.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/sockaddr.h>
#include <keel/udp.h>            /* KlUdpConfig — the dgram provider configure() arg */

#include "../src/socket.h"       /* kl_sock_socket / _bind / _set_nonblocking / _get_local_addr */

#include <string.h>
#include <stdalign.h>
#include <sys/socket.h>
#include <netinet/in.h>

static KlAllocator g_alloc;

/* recv recorder */
static int g_recv_calls; static unsigned char g_buf[2048]; static size_t g_len; static int g_has_peer;
static int g_has_local; static char g_local_ip[64];
static void on_recv(void *ud, const void *data, size_t len, const KlSockAddr *peer,
                    const KlSockAddr *local, unsigned flags) {
    (void)ud;
    g_recv_calls++; g_len = len; g_has_peer = (peer != NULL);
    g_has_local = (local != NULL) && (flags & KL_DGRAM_HAS_LOCAL);
    if (g_has_local) kl_sockaddr_format_ip(local, g_local_ip, sizeof(g_local_ip));
    if (len) memcpy(g_buf, data, len <= sizeof(g_buf) ? len : sizeof(g_buf));
}

/* prepare a datagram fd through the socket seam (socket + nonblocking + configure + optional bind) */
static KlSocketHandle prep_fd(const KlSocketProvider *sp, const char *bind_ip, int port) {
    KlSocketHandle fd = kl_sock_socket(sp, AF_INET, SOCK_DGRAM, 0);
    if (!kl_handle_valid(fd)) return fd;
    kl_sock_set_nonblocking(sp, fd);
    KlUdpConfig ucfg; memset(&ucfg, 0, sizeof(ucfg));
    const KlDatagramOps *dg = sp ? sp->dgram : kl_sockdef_dgram();
    (void)dg->configure(sp ? sp->context : NULL, fd, AF_INET, &ucfg);   /* best-effort sockopts */
    if (bind_ip) {
        KlSockAddr b; kl_sockaddr_parse(&b, bind_ip, (uint16_t)port);
        if (kl_sock_bind(sp, fd, &b) != 0) { kl_sock_close(sp, fd); return KL_INVALID_SOCKET; }
    }
    return fd;
}

/* prep a datagram fd with recv_tos enabled (IP_RECVTOS), returning the RX-capture mask the provider's
 * configure() actually accepted (→ KlDatagramConfig.accepted_rx_caps, which gates the completion recv's
 * TOS capture). Bound so it can receive. */
static KlSocketHandle prep_fd_recvtos(const KlSocketProvider *sp, const char *bind_ip, int port,
                                      unsigned *accepted) {
    KlSocketHandle fd = kl_sock_socket(sp, AF_INET, SOCK_DGRAM, 0);
    if (!kl_handle_valid(fd)) return fd;
    kl_sock_set_nonblocking(sp, fd);
    KlUdpConfig ucfg; memset(&ucfg, 0, sizeof(ucfg));
    ucfg.recv_tos = 1;
    const KlDatagramOps *dg = sp ? sp->dgram : kl_sockdef_dgram();
    unsigned caps = dg->configure(sp ? sp->context : NULL, fd, AF_INET, &ucfg);
    if (accepted) *accepted = caps;
    KlSockAddr b; kl_sockaddr_parse(&b, bind_ip, (uint16_t)port);
    if (kl_sock_bind(sp, fd, &b) != 0) { kl_sock_close(sp, fd); return KL_INVALID_SOCKET; }
    return fd;
}

/* recv-TOS recorder: capture kl_datagram_recv_tos() DURING the on_recv callback (the only valid window). */
static int g_rt_calls, g_rt_tos;
static const KlDatagram *g_rt_dg;
static void on_recv_tos(void *ud, const void *data, size_t len, const KlSockAddr *peer,
                        const KlSockAddr *local, unsigned flags) {
    (void)ud; (void)data; (void)len; (void)peer; (void)local; (void)flags;
    g_rt_calls++;
    g_rt_tos = kl_datagram_recv_tos(g_rt_dg);   /* read inside the delivery window */
}

static void pump_until(KlEventCtx *ctx, int *flag, int want, int ticks) {
    for (int i = 0; i < ticks && *flag < want; i++) kl_event_ctx_run(ctx, 16, 20);
}
static void pump_close(KlEventCtx *ctx, KlDatagram *dg, int ticks) {
    for (int i = 0; i < ticks && kl_datagram_close_state(dg) != KL_DGRAM_CLOSE_CLOSED; i++)
        kl_event_ctx_run(ctx, 16, 20);
}

UTEST(datagram_live, loopback_roundtrip_and_clean_close) {
    g_alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    const KlSocketProvider *sp = ctx.sockets;   /* NULL = default POSIX (pollcomp drives it overlapped) */

    KlSocketHandle rxfd = prep_fd(sp, "127.0.0.1", 0);
    ASSERT_TRUE(kl_handle_valid(rxfd));
    KlSockAddr local; ASSERT_EQ(0, kl_sock_get_local_addr(sp, rxfd, &local));
    int port = (int)kl_sockaddr_port(&local);
    ASSERT_NE(0, port);
    KlSocketHandle txfd = prep_fd(sp, NULL, 0);
    ASSERT_TRUE(kl_handle_valid(txfd));

    KlDatagram rx, tx; memset(&rx, 0, sizeof(rx)); memset(&tx, 0, sizeof(tx));
    KlDatagramConfig rc = { .ctx = &ctx, .alloc = &g_alloc, .sockets = sp, .fd = rxfd,
                            .send_slots = 4, .send_slot_cap = 1500, .recv_cap = 2048 };
    KlDatagramConfig tc = rc; tc.fd = txfd;
    ASSERT_EQ(0, kl_datagram_init(&rx, &rc));
    ASSERT_EQ(0, kl_datagram_init(&tx, &tc));

    g_recv_calls = 0;
    ASSERT_EQ(0, kl_datagram_recv_start(&rx, on_recv, NULL));

    KlSockAddr dest; kl_sockaddr_parse(&dest, "127.0.0.1", (uint16_t)port);
    const char *msg = "hello-datagram";
    KlDatagramMessage m = { .data = msg, .len = strlen(msg), .peer = &dest, .tos = -1 };
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&tx, &m));

    pump_until(&ctx, &g_recv_calls, 1, 100);
    ASSERT_EQ(1, g_recv_calls);
    ASSERT_EQ(strlen(msg), g_len);
    ASSERT_TRUE(g_has_peer);
    ASSERT_EQ(0, memcmp(g_buf, msg, strlen(msg)));

    /* clean close: graceful → drain → retire the outstanding recv → close the fd once → DETACHED */
    ASSERT_EQ(0, kl_datagram_close_begin(&rx));
    pump_close(&ctx, &rx, 100);
    ASSERT_EQ((int)KL_DGRAM_CLOSE_CLOSED, (int)kl_datagram_close_state(&rx));
    ASSERT_EQ((int)KL_DGRAM_DETACHED, (int)kl_datagram_close_result(&rx));
    ASSERT_EQ(0, kl_datagram_free(&rx));

    ASSERT_EQ(0, kl_datagram_close_begin(&tx));
    pump_close(&ctx, &tx, 100);
    ASSERT_EQ((int)KL_DGRAM_CLOSE_CLOSED, (int)kl_datagram_close_state(&tx));
    ASSERT_EQ(0, kl_datagram_free(&tx));

    kl_event_ctx_free(&ctx);
}

/* prep a datagram fd with recv_pktinfo enabled (so the socket delivers the local/dest addr cmsg). */
static KlSocketHandle prep_fd_pktinfo(const KlSocketProvider *sp, const char *bind_ip, int port) {
    KlSocketHandle fd = kl_sock_socket(sp, AF_INET, SOCK_DGRAM, 0);
    if (!kl_handle_valid(fd)) return fd;
    kl_sock_set_nonblocking(sp, fd);
    KlUdpConfig ucfg; memset(&ucfg, 0, sizeof(ucfg));
    ucfg.recv_pktinfo = 1;
    const KlDatagramOps *dg = sp ? sp->dgram : kl_sockdef_dgram();
    (void)dg->configure(sp ? sp->context : NULL, fd, AF_INET, &ucfg);
    KlSockAddr b; kl_sockaddr_parse(&b, bind_ip, (uint16_t)port);
    if (kl_sock_bind(sp, fd, &b) != 0) { kl_sock_close(sp, fd); return KL_INVALID_SOCKET; }
    return fd;
}

/* The completion recv captures the datagram's local (dest) address (dg_comp_arm requests
 * KL_DGRAM_RX_PKTINFO), consistent with the readiness recv — the capability the source-pinned reply
 * depends on. Backend-adaptive: BACKEND=pollcomp/iouring exercise the completion recv, default the
 * readiness recv. */
UTEST(datagram_live, completion_recv_captures_local) {
    g_alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    const KlSocketProvider *sp = ctx.sockets;

    KlSocketHandle rxfd = prep_fd_pktinfo(sp, "0.0.0.0", 0);   /* wildcard + pktinfo */
    ASSERT_TRUE(kl_handle_valid(rxfd));
    KlSockAddr la; ASSERT_EQ(0, kl_sock_get_local_addr(sp, rxfd, &la));
    int port = (int)kl_sockaddr_port(&la);
    KlSocketHandle txfd = prep_fd(sp, NULL, 0);
    ASSERT_TRUE(kl_handle_valid(txfd));

    KlDatagram rx, tx; memset(&rx, 0, sizeof(rx)); memset(&tx, 0, sizeof(tx));
    KlDatagramConfig rc = { .ctx = &ctx, .alloc = &g_alloc, .sockets = sp, .fd = rxfd,
                            .send_slots = 4, .send_slot_cap = 1500, .recv_cap = 2048 };
    KlDatagramConfig tc = rc; tc.fd = txfd;
    ASSERT_EQ(0, kl_datagram_init(&rx, &rc));
    ASSERT_EQ(0, kl_datagram_init(&tx, &tc));
    g_recv_calls = 0; g_has_local = 0;
    ASSERT_EQ(0, kl_datagram_recv_start(&rx, on_recv, NULL));

    KlSockAddr dest; kl_sockaddr_parse(&dest, "127.0.0.1", (uint16_t)port);
    const char *msg = "cap-local";
    KlDatagramMessage m = { .data = msg, .len = strlen(msg), .peer = &dest, .tos = -1 };
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&tx, &m));

    pump_until(&ctx, &g_recv_calls, 1, 100);
    ASSERT_EQ(1, g_recv_calls);
    ASSERT_TRUE(g_has_local);                     /* the completion recv captured the local (dest) addr */
    ASSERT_STREQ("127.0.0.1", g_local_ip);        /* the addr the datagram arrived on */

    ASSERT_EQ(0, kl_datagram_close_begin(&rx)); pump_close(&ctx, &rx, 100); ASSERT_EQ(0, kl_datagram_free(&rx));
    ASSERT_EQ(0, kl_datagram_close_begin(&tx)); pump_close(&ctx, &tx, 100); ASSERT_EQ(0, kl_datagram_free(&tx));
    kl_event_ctx_free(&ctx);
}

/* Raw recvmsg TOS oracle: read one datagram + its received TOS/traffic-class cmsg. Returns 1 if a
 * datagram was read, with *tos_out = the received TOS byte (or -1 if the platform delivered no TOS
 * cmsg). Aligned control storage (typed cmsghdr access). POSIX-only. */
#if defined(IP_RECVTOS)
static int recv_tos_oracle(int fd, unsigned char *buf, size_t buflen, size_t *outlen, int *tos_out) {
    _Alignas(struct cmsghdr) unsigned char cbuf[256];
    struct iovec iov = { buf, buflen };
    struct msghdr m; memset(&m, 0, sizeof(m));
    m.msg_iov = &iov; m.msg_iovlen = 1;
    m.msg_control = cbuf; m.msg_controllen = sizeof(cbuf);
    ssize_t n = recvmsg(fd, &m, 0);
    if (n < 0) return 0;
    *outlen = (size_t)n; *tos_out = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
        int is_tos = (c->cmsg_level == IPPROTO_IP) &&
                     (c->cmsg_type == IP_TOS || c->cmsg_type == IP_RECVTOS);
        if (!is_tos) continue;
        if (c->cmsg_len >= CMSG_LEN(sizeof(int))) { int v; memcpy(&v, CMSG_DATA(c), sizeof(v)); *tos_out = v & 0xff; }
        else if (c->cmsg_len >= CMSG_LEN(1))      { *tos_out = ((unsigned char *)CMSG_DATA(c))[0]; }
    }
    return 1;
}
#endif

/* Per-packet TOS through the backend: a KlDatagram send with tos >= 0 builds + carries the TOS cmsg on
 * the (overlapped, on completion backends) send; a RAW recvmsg peer with IP_RECVTOS reads the datagram
 * AND inspects the received TOS, asserting the requested DSCP/ECN actually egressed — so a backend that
 * silently omits the cmsg fails here. Backend-adaptive (pollcomp/io_uring completion, kqueue/epoll rdy). */
UTEST(datagram_live, completion_tos_send_verifies_tos) {
    g_alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    const KlSocketProvider *sp = ctx.sockets;

    KlSocketHandle rxfd = prep_fd(sp, "127.0.0.1", 0);   /* RAW peer — not a KlDatagram; recvmsg directly */
    ASSERT_TRUE(kl_handle_valid(rxfd));
    int rfd = (int)rxfd;
#if defined(IP_RECVTOS)
    int on = 1; (void)setsockopt(rfd, IPPROTO_IP, IP_RECVTOS, &on, sizeof(on));
#endif
    KlSockAddr la; ASSERT_EQ(0, kl_sock_get_local_addr(sp, rxfd, &la));
    int port = (int)kl_sockaddr_port(&la);
    KlSocketHandle txfd = prep_fd(sp, NULL, 0);
    ASSERT_TRUE(kl_handle_valid(txfd));

    KlDatagram tx; memset(&tx, 0, sizeof(tx));
    KlDatagramConfig tc = { .ctx = &ctx, .alloc = &g_alloc, .sockets = sp, .fd = txfd,
                            .send_slots = 4, .send_slot_cap = 1500, .recv_cap = 2048,
                            .want_caps = KL_DGRAM_CAP_TOS };
    ASSERT_EQ(0, kl_datagram_init_ex(&tx, &tc, 0));

    KlSockAddr dest; kl_sockaddr_parse(&dest, "127.0.0.1", (uint16_t)port);
    const char *msg = "tos-mark";
    KlDatagramMessage m = { .data = msg, .len = strlen(msg), .peer = &dest, .tos = 0x28 };  /* DSCP marking */
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&tx, &m));

    unsigned char pbuf[128]; size_t plen = 0; int tos = -1, got = 0;
    for (int i = 0; i < 100 && !got; i++) {
        kl_event_ctx_run(&ctx, 16, 20);           /* drive the send egress (completion drain / readiness) */
#if defined(IP_RECVTOS)
        got = recv_tos_oracle(rfd, pbuf, sizeof(pbuf), &plen, &tos);
#else
        ssize_t n = recv(rfd, pbuf, sizeof(pbuf), 0);
        if (n > 0) { plen = (size_t)n; got = 1; }
#endif
    }
    ASSERT_EQ(1, got);
    ASSERT_EQ(strlen(msg), plen);
    ASSERT_EQ(0, memcmp(pbuf, msg, plen));
#if defined(IP_RECVTOS)
#if defined(__linux__)
    ASSERT_TRUE(tos >= 0);                          /* Linux delivers the RX TOS — a dropped cmsg is caught */
#endif
    if (tos >= 0) ASSERT_EQ(0x28, tos);             /* the requested DSCP egressed (where the platform delivers) */
#endif

    ASSERT_EQ(0, kl_datagram_close_begin(&tx)); pump_close(&ctx, &tx, 100); ASSERT_EQ(0, kl_datagram_free(&tx));
    kl_sock_close(sp, rxfd);
    kl_event_ctx_free(&ctx);
}

/* M6.0a: kl_datagram_set_tos sets the SOCKET-DEFAULT TOS — a plain send (no per-message tos) then
 * egresses with that default. A RAW recvmsg peer with IP_RECVTOS reads the received TOS and asserts the
 * socket default actually applied. Backend-adaptive (pollcomp/io_uring completion, kqueue/epoll rdy) —
 * the completion send path applies the kernel socket default just like readiness. */
UTEST(datagram_live, set_tos_socket_default_egress_verifies) {
    g_alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    const KlSocketProvider *sp = ctx.sockets;

    KlSocketHandle rxfd = prep_fd(sp, "127.0.0.1", 0);   /* RAW peer — recvmsg directly */
    ASSERT_TRUE(kl_handle_valid(rxfd));
    int rfd = (int)rxfd;
#if defined(IP_RECVTOS)
    int on = 1; (void)setsockopt(rfd, IPPROTO_IP, IP_RECVTOS, &on, sizeof(on));
#endif
    KlSockAddr la; ASSERT_EQ(0, kl_sock_get_local_addr(sp, rxfd, &la));
    int port = (int)kl_sockaddr_port(&la);
    KlSocketHandle txfd = prep_fd(sp, NULL, 0);
    ASSERT_TRUE(kl_handle_valid(txfd));

    KlDatagram tx; memset(&tx, 0, sizeof(tx));
    KlDatagramConfig tc = { .ctx = &ctx, .alloc = &g_alloc, .sockets = sp, .fd = txfd,
                            .send_slots = 4, .send_slot_cap = 1500, .recv_cap = 2048 };
    ASSERT_EQ(0, kl_datagram_init(&tx, &tc));
    ASSERT_EQ(0, kl_datagram_set_tos(&tx, 0x30));        /* socket-default DSCP mark */

    KlSockAddr dest; kl_sockaddr_parse(&dest, "127.0.0.1", (uint16_t)port);
    const char *msg = "set-tos-default";
    KlDatagramMessage m = { .data = msg, .len = strlen(msg), .peer = &dest, .tos = -1 };  /* NO per-msg tos */
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&tx, &m));

    unsigned char pbuf[128]; size_t plen = 0; int tos = -1, got = 0;
    for (int i = 0; i < 100 && !got; i++) {
        kl_event_ctx_run(&ctx, 16, 20);
#if defined(IP_RECVTOS)
        got = recv_tos_oracle(rfd, pbuf, sizeof(pbuf), &plen, &tos);
#else
        ssize_t n = recv(rfd, pbuf, sizeof(pbuf), 0);
        if (n > 0) { plen = (size_t)n; got = 1; }
#endif
    }
    ASSERT_EQ(1, got);
    ASSERT_EQ(strlen(msg), plen);
    ASSERT_EQ(0, memcmp(pbuf, msg, plen));
#if defined(IP_RECVTOS)
#if defined(__linux__)
    ASSERT_TRUE(tos >= 0);
#endif
    if (tos >= 0) ASSERT_EQ(0x30, tos);                 /* the socket default egressed */
#endif

    ASSERT_EQ(0, kl_datagram_close_begin(&tx)); pump_close(&ctx, &tx, 100); ASSERT_EQ(0, kl_datagram_free(&tx));
    kl_sock_close(sp, rxfd);
    kl_event_ctx_free(&ctx);
}

/* M6.0a: receive-TOS through the REAL capture seam. A KlDatagram rx with RX_TOS enabled receives a
 * datagram sent with a per-packet TOS mark; kl_datagram_recv_tos() (read inside on_recv) returns the mark.
 * Backend-adaptive via kl_event_ctx_init: on BACKEND=pollcomp/iouring this exercises the COMPLETION recv
 * cmsg parse (dg_comp_arm requests RX_TOS → backend fills ev->tos → dispatch stamps the inbound slot); on
 * a default build it exercises the readiness pull. NOT a fabricated-metadata accessor test — a backend
 * that drops the RX TOS cmsg fails here (on Linux, where the RX TOS is reliably delivered). */
UTEST(datagram_live, recv_tos_capture_verifies) {
    g_alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    const KlSocketProvider *sp = ctx.sockets;

    unsigned accepted = 0;
    KlSocketHandle rxfd = prep_fd_recvtos(sp, "127.0.0.1", 0, &accepted);
    ASSERT_TRUE(kl_handle_valid(rxfd));
    KlSockAddr la; ASSERT_EQ(0, kl_sock_get_local_addr(sp, rxfd, &la));
    int port = (int)kl_sockaddr_port(&la);
    KlSocketHandle txfd = prep_fd(sp, NULL, 0);
    ASSERT_TRUE(kl_handle_valid(txfd));

    KlDatagram rx, tx; memset(&rx, 0, sizeof(rx)); memset(&tx, 0, sizeof(tx));
    KlDatagramConfig rc = { .ctx = &ctx, .alloc = &g_alloc, .sockets = sp, .fd = rxfd,
                            .send_slots = 4, .send_slot_cap = 1500, .recv_cap = 2048,
                            .accepted_rx_caps = accepted };   /* carries RX_TOS → gates the capture */
    KlDatagramConfig tc = { .ctx = &ctx, .alloc = &g_alloc, .sockets = sp, .fd = txfd,
                            .send_slots = 4, .send_slot_cap = 1500, .recv_cap = 2048,
                            .want_caps = KL_DGRAM_CAP_TOS };
    ASSERT_EQ(0, kl_datagram_init(&rx, &rc));
    ASSERT_EQ(0, kl_datagram_init_ex(&tx, &tc, 0));

    g_rt_calls = 0; g_rt_tos = -2; g_rt_dg = &rx;
    ASSERT_EQ(0, kl_datagram_recv_start(&rx, on_recv_tos, NULL));

    KlSockAddr dest; kl_sockaddr_parse(&dest, "127.0.0.1", (uint16_t)port);
    const char *msg = "rx-tos";
    KlDatagramMessage m = { .data = msg, .len = strlen(msg), .peer = &dest, .tos = 0x28 };
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&tx, &m));

    pump_until(&ctx, &g_rt_calls, 1, 100);
    ASSERT_EQ(1, g_rt_calls);
#if defined(__linux__)
    ASSERT_EQ(0x28, g_rt_tos);            /* Linux reliably delivers the RX TOS cmsg */
#else
    /* Other platforms may not deliver an RX TOS cmsg; if they do, it must be the sent mark, else -1. */
    ASSERT_TRUE(g_rt_tos == 0x28 || g_rt_tos == -1);
#endif

    ASSERT_EQ(0, kl_datagram_close_begin(&rx)); pump_close(&ctx, &rx, 100); ASSERT_EQ(0, kl_datagram_free(&rx));
    ASSERT_EQ(0, kl_datagram_close_begin(&tx)); pump_close(&ctx, &tx, 100); ASSERT_EQ(0, kl_datagram_free(&tx));
    kl_event_ctx_free(&ctx);
}

UTEST_MAIN();
