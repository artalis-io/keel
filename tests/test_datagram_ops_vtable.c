/*
 * test_datagram_ops_vtable.c: the datagram provider vtable (KlDatagramOps) is validated PATH-SPECIFIC,
 * not by a single global rule. There is no "send && recv && configure" requirement; each entry point
 * requires only the ops it actually calls:
 *   - adopt (kl_datagram_init) on a readiness loop calls send + recv, never configure -> requires
 *     send + recv only; a provider without configure still adopts.
 *   - open (kl_datagram_socket_init -> kl_datagram_open) calls configure -> requires configure.
 *   - completion mode uses the completion seam (kl_comp_post_dgram_*), not the readiness send/recv, so
 *     a provider missing send/recv still initializes.
 * This test proves those four distinct requirements through the public boundary. The validation itself
 * already exists (datagram.c / datagram_open.c); this locks the behavior in.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "utest.h"

#ifndef _WIN32
#include <keel/datagram.h>
#include <keel/datagram_detail.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/sockaddr.h>
#include <keel/error.h>
#include "../src/completion.h"     /* KlCompletionOps + KlDgramSendOp/RecvOp */
#include "../src/datagram_life.h"  /* KlDgramOpKind / KlDgramRetireResult */
#include "../src/socket.h"         /* KlSocketProvider / KlSocketOps */
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

/* ── mock datagram provider (one op omitted per test) ──────── */
static kl_ssize_t d_send(void *c, KlSocketHandle fd, const void *data, size_t len,
                         const KlSockAddr *dest, const KlSockAddr *src, int tos) {
    (void)c;(void)fd;(void)data;(void)dest;(void)src;(void)tos; return (kl_ssize_t)len;
}
static kl_ssize_t d_recv(void *c, KlSocketHandle fd, void *buf, size_t buflen,
                         KlSockAddr *src, KlDgramRxMeta *meta) {
    (void)c;(void)fd;(void)buf;(void)buflen;(void)src;(void)meta; return -1;   /* would-block */
}
static uint32_t d_configure(void *c, KlSocketHandle fd, int family,
                            const struct KlDatagramSocketConfig *cfg) {
    (void)c;(void)fd;(void)family;(void)cfg; return 0;
}

enum { OMIT_NONE = -1, OMIT_SEND, OMIT_RECV, OMIT_CONFIGURE };

static KlDatagramOps    g_dgram_ops;
static KlSocketOps      g_sock_ops = { .name = "dgram-mock" };
static KlSocketProvider g_prov;

static const KlSocketProvider *mock_provider(int omit) {
    memset(&g_dgram_ops, 0, sizeof(g_dgram_ops));
    g_dgram_ops.send = d_send;
    g_dgram_ops.recv = d_recv;
    g_dgram_ops.configure = d_configure;
    switch (omit) {
        case OMIT_SEND:      g_dgram_ops.send = NULL; break;
        case OMIT_RECV:      g_dgram_ops.recv = NULL; break;
        case OMIT_CONFIGURE: g_dgram_ops.configure = NULL; break;
        default: break;
    }
    g_prov.ops = &g_sock_ops;
    g_prov.context = NULL;
    g_prov.capabilities = KL_SOCK_CAP_NATIVE_FD | KL_SOCK_CAP_DATAGRAM;
    g_prov.dgram = &g_dgram_ops;
    return &g_prov;
}

/* ── hand-built completion loop (caps advertise COMPLETION) ──
 * Completion-only: the KEEL_NO_COMPLETION build stubs the completion axis (kl_comp_post_dgram_*) to
 * abort() (completion_absent.c), so the completion case below cannot run there. It is compiled out
 * under KEEL_NO_COMPLETION; the readiness/path-specific cases above still run. */
#ifndef KEEL_NO_COMPLETION
static int cmp_post_send(struct KlEventCtx *ctx, const KlDgramSendOp *op) { (void)ctx;(void)op; return 0; }
static int cmp_post_recv(struct KlEventCtx *ctx, const KlDgramRecvOp *op) { (void)ctx;(void)op; return 0; }
static int cmp_cancel(struct KlEventCtx *ctx, struct KlDgramLife *life, KlDgramOpKind kind) {
    (void)ctx;(void)life;(void)kind; return 0;
}
static KlDgramRetireResult cmp_retire(struct KlEventCtx *ctx, struct KlDgramLife *life,
                                      KlDgramOpKind kind, int *terr) {
    (void)ctx;(void)life;(void)kind; if (terr) *terr = 0; return KL_DGRAM_RETIRE_RETIRED;
}
static const KlCompletionOps MIN_COMP = {
    .post_dgram_send = cmp_post_send, .post_dgram_recv = cmp_post_recv,
    .cancel_dgram = cmp_cancel, .retire_dgram = cmp_retire,
};
static unsigned cmp_caps(const KlEventLoop *l) { (void)l; return KL_EVENT_CAP_COMPLETION; }
static int  cmp_add(KlEventLoop *l, KlSocketHandle fd, KlEventMask m, void *u) { (void)l;(void)fd;(void)m;(void)u; return 0; }
static int  cmp_del(KlEventLoop *l, KlSocketHandle fd) { (void)l;(void)fd; return 0; }
static const KlEventOps CMP_EVOPS = { .caps = cmp_caps, .completion = &MIN_COMP, .add = cmp_add, .del = cmp_del };
#endif /* !KEEL_NO_COMPLETION */

static KlSocketHandle mk_udp(void) { return (KlSocketHandle)socket(AF_INET, SOCK_DGRAM, 0); }

static KlDatagramConfig cfg_adopt(KlEventCtx *ctx, KlAllocator *a,
                                  const KlSocketProvider *prov, KlSocketHandle fd) {
    KlDatagramConfig c; memset(&c, 0, sizeof(c));
    c.ctx = ctx; c.alloc = a; c.sockets = prov; c.fd = fd;
    c.send_slots = 4; c.send_slot_cap = 1500; c.recv_cap = 2048; c.want_caps = 0;
    return c;
}

/* graceful synchronous teardown of a fresh datagram (no armed ops); 0 on success. */
static int teardown(KlDatagram *dg) {
    if (kl_datagram_close_begin(dg) != 0) return -1;
    return kl_datagram_free(dg);
}

/* ADOPT works without configure: the readiness adopt path calls send + recv, never configure. */
UTEST(datagram_ops_vtable, adopt_without_configure_ok) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev; ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);   /* readiness backend */
    const KlSocketProvider *prov = mock_provider(OMIT_CONFIGURE);
    KlSocketHandle fd = mk_udp();
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_adopt(&ev, &alloc, prov, fd);
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));   /* configure not required on the adopt path */
    ASSERT_EQ(0, teardown(&dg));
    kl_event_ctx_free(&ev);
}

/* READINESS rejects a missing send, and a missing recv (KL_ERR_INVALID_ARG); the fd is not adopted. */
UTEST(datagram_ops_vtable, readiness_rejects_missing_send_or_recv) {
    int omits[] = { OMIT_SEND, OMIT_RECV };
    for (size_t i = 0; i < sizeof(omits) / sizeof(omits[0]); i++) {
        KlAllocator alloc = kl_allocator_default();
        KlEventCtx ev; ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);
        const KlSocketProvider *prov = mock_provider(omits[i]);
        KlSocketHandle fd = mk_udp();
        KlDatagram dg; memset(&dg, 0, sizeof(dg));
        KlDatagramConfig c = cfg_adopt(&ev, &alloc, prov, fd);
        ASSERT_EQ(-1, kl_datagram_init(&dg, &c));
        ASSERT_EQ((int)kl_datagram_last_error(&dg), (int)KL_ERR_INVALID_ARG);
        close((int)fd);   /* not adopted: the caller still owns it */
        kl_event_ctx_free(&ev);
    }
}

/* OPEN rejects a missing configure (KL_ERR_INVALID_ARG), before any fd is created. */
UTEST(datagram_ops_vtable, open_rejects_missing_configure) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev; ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);
    const KlSocketProvider *prov = mock_provider(OMIT_CONFIGURE);
    KlDatagramSocketConfig sc; memset(&sc, 0, sizeof(sc));
    sc.ctx = &ev; sc.sockets = prov; sc.alloc = &alloc;
    sc.family = AF_INET;
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    ASSERT_EQ(-1, kl_datagram_socket_init(&dg, &sc));
    ASSERT_EQ((int)kl_datagram_last_error(&dg), (int)KL_ERR_INVALID_ARG);
    kl_event_ctx_free(&ev);
}

/* COMPLETION mode does not require the readiness send/recv ops: init succeeds without them.
 * Completion-only (skipped under KEEL_NO_COMPLETION, where the completion axis aborts). */
#ifndef KEEL_NO_COMPLETION
UTEST(datagram_ops_vtable, completion_does_not_require_send_recv) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev; memset(&ev, 0, sizeof(ev));
    ev.loop.ops = &CMP_EVOPS;   /* hand-built completion loop */
    ev.alloc = &alloc;
    /* a provider whose dgram lacks BOTH send and recv; completion never calls them. */
    memset(&g_dgram_ops, 0, sizeof(g_dgram_ops));   /* no send, no recv, no configure */
    g_prov.ops = &g_sock_ops; g_prov.context = NULL;
    g_prov.capabilities = KL_SOCK_CAP_NATIVE_FD | KL_SOCK_CAP_DATAGRAM;
    g_prov.dgram = &g_dgram_ops;
    KlSocketHandle fd = mk_udp();
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_adopt(&ev, &alloc, &g_prov, fd);
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));   /* completion path: send/recv not required */
    ASSERT_EQ(0, teardown(&dg));
}
#endif /* !KEEL_NO_COMPLETION */

UTEST_MAIN();

#else  /* _WIN32: POSIX UDP sockets */
UTEST_MAIN();
#endif
