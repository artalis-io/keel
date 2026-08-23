/*
 * smoke_datagram.c — public KlDatagram link + loopback roundtrip smoke.
 *
 * NOT a utest suite — a standalone program built by the `smoke-datagram` Makefile target, the
 * Windows-portable analogue of smoke_udp for the PUBLIC KlDatagram API. It preps two datagram fds
 * THROUGH the socket provider (the KlDatagram contract: caller socket()/configure()/bind before init),
 * runs one real datagram over 127.0.0.1, and tears down cleanly. Built over the default provider so it
 * is winsock-aware on Windows; on BACKEND=iocp it is the runtime proof of the facade's IOCP fd↔loop
 * registration (kl_event_add/CreateIoCompletionPort) — the completion path pollcomp/io_uring
 * cannot exercise (their kl_event_add is inert). Runs on POSIX (kqueue/epoll readiness) too.
 */

#include <keel/datagram.h>
#include <keel/datagram_detail.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/net.h>       /* winsock (Windows) / AF_INET + inet_pton */

#include "../src/socket.h"  /* kl_sock_socket / _bind / _set_nonblocking / _get_local_addr / _close */

#include <string.h>
#include <stdio.h>

#define SMOKE_MSG "keel-datagram-smoke"

static int    g_got;
static char   g_buf[256];
static size_t g_len;

static void on_recv(void *ud, const void *data, size_t len, const KlSockAddr *peer,
                    const KlSockAddr *local, unsigned flags) {
    (void)ud; (void)peer; (void)local; (void)flags;
    if (len > sizeof(g_buf)) len = sizeof(g_buf);
    memcpy(g_buf, data, len);
    g_len = len; g_got++;
}

/* Prepare a datagram fd through the socket seam (the KlDatagram contract). */
static KlSocketHandle prep_fd(const KlSocketProvider *sp, int do_bind, uint16_t *out_port) {
    KlSocketHandle fd = kl_sock_socket(sp, AF_INET, SOCK_DGRAM, 0);
    if (!kl_handle_valid(fd)) return fd;
    kl_sock_set_nonblocking(sp, fd);
    KlDatagramSocketConfig ucfg; memset(&ucfg, 0, sizeof(ucfg));
    const KlDatagramOps *dg = sp ? sp->dgram : kl_sockdef_dgram();
    (void)dg->configure(sp ? sp->context : NULL, fd, AF_INET, &ucfg);
    if (do_bind) {
        KlSockAddr b; kl_sockaddr_parse(&b, "127.0.0.1", 0);
        if (kl_sock_bind(sp, fd, &b) != 0) { kl_sock_close(sp, fd); return KL_INVALID_SOCKET; }
        KlSockAddr local;
        if (kl_sock_get_local_addr(sp, fd, &local) != 0) { kl_sock_close(sp, fd); return KL_INVALID_SOCKET; }
        *out_port = kl_sockaddr_port(&local);
    }
    return fd;
}

static void pump_close(KlEventCtx *ctx, KlDatagram *dg) {
    for (int i = 0; i < 200 && kl_datagram_close_state(dg) != KL_DGRAM_CLOSE_CLOSED; i++)
        kl_event_ctx_run(ctx, 16, 10);
}

int main(void) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    if (kl_event_ctx_init(&ctx, &alloc) != 0) { fprintf(stderr, "smoke-datagram: ctx init failed\n"); return 1; }
    const KlSocketProvider *sp = ctx.sockets;

    uint16_t port = 0, txport = 0;
    KlSocketHandle rxfd = prep_fd(sp, 1, &port);
    KlSocketHandle txfd = prep_fd(sp, 1, &txport);
    if (!kl_handle_valid(rxfd) || !kl_handle_valid(txfd) || port == 0) {
        fprintf(stderr, "smoke-datagram: fd prep failed (port=%u)\n", (unsigned)port); return 1;
    }

    KlDatagram rx, tx; memset(&rx, 0, sizeof(rx)); memset(&tx, 0, sizeof(tx));
    KlDatagramConfig rc = { .ctx = &ctx, .alloc = &alloc, .sockets = sp, .fd = rxfd,
                            .send_slots = 4, .send_slot_cap = 1500, .recv_cap = 2048 };
    /* tx grants source-pin + TOS so the second send drives the completion backend's overlapped
     * cmsg path (on IOCP: WSASendMsg with an op-resident WSAMSG/WSABUF/name/control — §1b), not the
     * plain WSASendTo. */
    KlDatagramConfig tc = rc; tc.fd = txfd;
    tc.want_caps = KL_DGRAM_CAP_SOURCE_PIN | KL_DGRAM_CAP_TOS;
    if (kl_datagram_init(&rx, &rc) != 0 || kl_datagram_init_ex(&tx, &tc, 0) != 0) {
        fprintf(stderr, "smoke-datagram: init failed\n"); return 1;
    }
    if (kl_datagram_recv_start(&rx, on_recv, NULL) != 0) { fprintf(stderr, "smoke-datagram: recv_start failed\n"); return 1; }

    KlSockAddr dst;
    unsigned char ipb[4]; inet_pton(AF_INET, "127.0.0.1", ipb); kl_sockaddr_from_ipv4(&dst, ipb, port);

    /* (1) plain send — the WSASendTo overlapped path. */
    KlDatagramMessage m = { .data = SMOKE_MSG, .len = sizeof(SMOKE_MSG) - 1, .peer = &dst, .tos = -1 };
    if (kl_datagram_send(&tx, &m) != KL_DATAGRAM_ACCEPTED) { fprintf(stderr, "smoke-datagram: send refused\n"); return 1; }
    for (int i = 0; i < 200 && g_got < 1; i++) kl_event_ctx_run(&ctx, 16, 10);
    int ok = (g_got == 1 && g_len == sizeof(SMOKE_MSG) - 1 && memcmp(g_buf, SMOKE_MSG, g_len) == 0);

    /* (2) source-pinned + TOS send — the overlapped cmsg (WSASendMsg) path; its op-resident descriptor
     * set must survive the post until the terminal completion retires it exactly once (§1b lifetime). */
    KlSockAddr loc; kl_sockaddr_parse(&loc, "127.0.0.1", 0);
    KlDatagramMessage cm = { .data = SMOKE_MSG, .len = sizeof(SMOKE_MSG) - 1,
                             .peer = &dst, .local = &loc, .tos = 0x28 };
    KlDatagramSendStatus sr = kl_datagram_send(&tx, &cm);
    if (sr != KL_DATAGRAM_ACCEPTED) { fprintf(stderr, "smoke-datagram: control send refused (%d)\n", (int)sr); return 1; }
    for (int i = 0; i < 200 && g_got < 2; i++) kl_event_ctx_run(&ctx, 16, 10);
    int ctrl_ok = (g_got == 2 && g_len == sizeof(SMOKE_MSG) - 1 && memcmp(g_buf, SMOKE_MSG, g_len) == 0);

    /* (3) §1b cancellation / late-completion lifetime: post a source-pin control send, then close the
     * datagram WITHOUT draining the send completion. The op-resident descriptor set (on IOCP: WSAMSG,
     * WSABUF, name, control — kernel-owned while the overlapped send is in flight) must stay valid until
     * the close coordinator cancels and drains the terminal completion, retiring the single op exactly
     * once → DETACHED, no UAF / no leak. Runs on every completion backend (IOCP is the target gate). */
    KlDatagram tx2; memset(&tx2, 0, sizeof(tx2));
    uint16_t txport2 = 0;
    KlSocketHandle txfd2 = prep_fd(sp, 1, &txport2);
    KlDatagramConfig tc2 = rc; tc2.fd = txfd2; tc2.want_caps = KL_DGRAM_CAP_SOURCE_PIN | KL_DGRAM_CAP_TOS;
    int cancel_ok = 0;
    if (kl_handle_valid(txfd2) && kl_datagram_init_ex(&tx2, &tc2, 0) == 0) {
        KlDatagramMessage cm2 = { .data = SMOKE_MSG, .len = sizeof(SMOKE_MSG) - 1,
                                  .peer = &dst, .local = &loc, .tos = 0x28 };
        KlDatagramSendStatus sr2 = kl_datagram_send(&tx2, &cm2);
        kl_datagram_close_begin(&tx2);          /* close before pumping — send op still in flight */
        pump_close(&ctx, &tx2);
        cancel_ok = (sr2 == KL_DATAGRAM_ACCEPTED &&
                     kl_datagram_close_state(&tx2) == KL_DGRAM_CLOSE_CLOSED &&
                     kl_datagram_close_result(&tx2) == KL_DGRAM_DETACHED);
        kl_datagram_free(&tx2);
    } else if (kl_handle_valid(txfd2)) {
        kl_sock_close(sp, txfd2);
    }

    /* clean close: graceful → retire → deregister → close fd once → DETACHED (the registration lifecycle) */
    kl_datagram_close_begin(&rx); pump_close(&ctx, &rx);
    kl_datagram_close_begin(&tx); pump_close(&ctx, &tx);
    int detached = (kl_datagram_close_result(&rx) == KL_DGRAM_DETACHED &&
                    kl_datagram_close_state(&tx) == KL_DGRAM_CLOSE_CLOSED);
    kl_datagram_free(&rx); kl_datagram_free(&tx);
    kl_event_ctx_free(&ctx);

    if (!ok)        { fprintf(stderr, "smoke-datagram: roundtrip FAILED (got=%d len=%zu)\n", g_got, g_len); return 1; }
    if (!ctrl_ok)   { fprintf(stderr, "smoke-datagram: source-pin+TOS control send FAILED (got=%d)\n", g_got); return 1; }
    if (!cancel_ok) { fprintf(stderr, "smoke-datagram: control-send cancel/late-completion lifetime FAILED\n"); return 1; }
    if (!detached)  { fprintf(stderr, "smoke-datagram: clean close FAILED (rx not DETACHED)\n"); return 1; }
    printf("smoke-datagram: roundtrip + control send + cancel-lifetime + clean close OK\n");
    return 0;
}
