/*
 * test_datagram_batch.c — datagram M5.1 scaffolding (the batch object lifecycle, capability bits +
 * accepted_rx_caps masking, completion-creation rules E2, overflow-safe allocation + allocation-failure
 * unwind, KL_IO_UNSUPPORTED mapping) + M5.2a send batch (kl_datagram_send_batch: all-delivered,
 * partial accept, TOO_LARGE + owner/direction validation, the recoverable per-datagram drop, and
 * backpressure draining via the writable edge) + M5.2b GSO (kl_datagram_send_gso: the queued FIFO
 * group — delivery via one send_gso or the per-segment fallback, validation/bounds, gso_busy lifetime
 * + one-group-per-batch, and close/discard clearing gso_busy). Whitebox (internal batch + core layout).
 * Recv batching lands in M5.3.
 */
#include "../vendor/utest.h"

#include <keel/datagram.h>
#include <keel/datagram_detail.h>
#include <keel/datagram_batch.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/udp.h>            /* KlUdpConfig — the provider configure() arg */

#include "../src/socket.h"       /* kl_sock_* seam, kl_sockdef_io_status, kl_socket_provider_posix */
#include "../src/event_caps.h"   /* kl_event_caps — the gating-provider drop test is readiness-only */
#include "../src/datagram_core.h"/* KlDgramCore — whitebox accepted_rx_caps */
#include "../src/datagram_batch.h"/* struct KlDatagramBatch — whitebox blocks/owner/dir */
#include "../src/datagram_open.h"/* kl_datagram_teardown — the P1-1 reentrancy test */

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>

/* ── a counting allocator: injects failure at the Nth malloc + tracks outstanding allocations ──── */
typedef struct { KlAllocator base; int calls; int fail_at; int live; } CountAlloc;
static void *ca_malloc(void *ctx, size_t n) {
    CountAlloc *c = ctx;
    c->calls++;
    if (c->fail_at > 0 && c->calls == c->fail_at) return NULL;   /* inject failure at the Nth malloc */
    void *p = c->base.malloc(c->base.ctx, n);
    if (p) c->live++;
    return p;
}
static void *ca_realloc(void *ctx, void *p, size_t o, size_t n) {
    CountAlloc *c = ctx;
    return c->base.realloc(c->base.ctx, p, o, n);   /* unused by batch create/free */
}
static void ca_free(void *ctx, void *p, size_t n) {
    CountAlloc *c = ctx;
    if (p) c->live--;
    c->base.free(c->base.ctx, p, n);
}
static KlAllocator ca_init(CountAlloc *c) {
    memset(c, 0, sizeof(*c));
    c->base = kl_allocator_default();
    KlAllocator a = { ca_malloc, ca_realloc, ca_free, c };
    return a;
}

/* ── mock provider datagram vtables: a COMPLETE batch set + PARTIAL (leak-prone) variants ──────── */
static void *mock_rx_new(KlAllocator *a, int n, size_t bufsz) { (void)n; (void)bufsz; return kl_malloc(a, sizeof(int)); }
static void  mock_rx_free(KlAllocator *a, void *b) { kl_free(a, b, sizeof(int)); }
static void *mock_tx_new(KlAllocator *a, int n) { (void)n; return kl_malloc(a, sizeof(int)); }
static void  mock_tx_free(KlAllocator *a, void *b) { kl_free(a, b, sizeof(int)); }
static int   mock_recv_batch(void *c, KlSocketHandle f, void *rb, KlDgramRxSlot *s, int m) {
    (void)c; (void)f; (void)rb; (void)s; (void)m; return 0;   /* stub — never called in M5.1 */
}
static int   mock_send_batch(void *c, KlSocketHandle f, void *tb, const KlDgramTxDesc *d, int n) {
    (void)c; (void)f; (void)tb; (void)d; (void)n; return 0;   /* stub */
}
/* Complete: both directions natively batch-capable (new + free + recv/send_batch). */
static const KlDatagramOps MOCK_COMPLETE = {
    .rx_batch_new = mock_rx_new, .rx_batch_free = mock_rx_free, .recv_batch = mock_recv_batch,
    .tx_batch_new = mock_tx_new, .tx_batch_free = mock_tx_free, .send_batch = mock_send_batch,
};
/* RX has new + recv_batch but NO free → must NOT allocate a block (would leak). */
static const KlDatagramOps MOCK_RX_NOFREE = { .rx_batch_new = mock_rx_new, .recv_batch = mock_recv_batch };
/* TX has new + free but NO send_batch → an unusable block → must NOT allocate it. */
static const KlDatagramOps MOCK_TX_NOSEND = { .tx_batch_new = mock_tx_new, .tx_batch_free = mock_tx_free };

/* Prepare a datagram fd through the socket seam (the KlDatagram contract). */
static KlSocketHandle prep_fd(const KlSocketProvider *sp) {
    KlSocketHandle fd = kl_sock_socket(sp, AF_INET, SOCK_DGRAM, 0);
    if (!kl_handle_valid(fd)) return fd;
    kl_sock_set_nonblocking(sp, fd);
    KlUdpConfig ucfg; memset(&ucfg, 0, sizeof(ucfg));
    const KlDatagramOps *dg = sp ? sp->dgram : kl_sockdef_dgram();
    (void)dg->configure(sp ? sp->context : NULL, fd, AF_INET, &ucfg);
    KlSockAddr b; kl_sockaddr_parse(&b, "127.0.0.1", 0);
    if (kl_sock_bind(sp, fd, &b) != 0) { kl_sock_close(sp, fd); return KL_INVALID_SOCKET; }
    return fd;
}

/* ── The KL_IO_UNSUPPORTED mapping (M5.1) ──────────────────────────────────────────────────────── */
UTEST(dgram_batch, io_status_unsupported_mapping) {
    errno = EOPNOTSUPP;
    ASSERT_EQ(KL_IO_UNSUPPORTED, kl_sockdef_io_status());
    errno = EAGAIN;
    ASSERT_EQ(KL_IO_WOULD_BLOCK, kl_sockdef_io_status());   /* the pre-existing values are unchanged */
}

/* ── accepted_rx_caps is masked to known KL_DGRAM_RX_* bits + stored on the core ──────────────── */
UTEST(dgram_batch, accepted_rx_caps_masked_and_stored) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &a));
    const KlSocketProvider *sp = ctx.sockets;
    KlSocketHandle fd = prep_fd(sp);
    ASSERT_TRUE(kl_handle_valid(fd));

    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig cfg = { .ctx = &ctx, .alloc = &a, .sockets = sp, .fd = fd,
                             .send_slots = 4, .send_slot_cap = 1500, .recv_cap = 2048,
                             .accepted_rx_caps = 0xFFFFFFFFu };   /* everything → must be masked down */
    ASSERT_EQ(0, kl_datagram_init_ex(&dg, &cfg, 0));
    ASSERT_EQ((unsigned)(KL_DGRAM_RX_PKTINFO | KL_DGRAM_RX_GRO | KL_DGRAM_RX_TOS),
              dg.core->accepted_rx_caps);

    ASSERT_EQ(0, kl_datagram_close_cancel(&dg));
    ASSERT_EQ(0, kl_datagram_free(&dg));
    kl_event_ctx_free(&ctx);
}

/* ── create validation + the cap↔provider-block coupling + completion rules (E2) ────────────────── */
UTEST(dgram_batch, create_validation_and_rules) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &a));
    const KlSocketProvider *sp = ctx.sockets;
    KlSocketHandle fd = prep_fd(sp);
    ASSERT_TRUE(kl_handle_valid(fd));

    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig cfg = { .ctx = &ctx, .alloc = &a, .sockets = sp, .fd = fd,
                             .send_slots = 4, .send_slot_cap = 1500, .recv_cap = 2048 };
    ASSERT_EQ(0, kl_datagram_init_ex(&dg, &cfg, 0));
    unsigned pcaps = kl_datagram_provider_caps(&dg);
    int orig_completion = dg.completion;

    /* invalid args → NULL (before any allocation) */
    ASSERT_TRUE(kl_datagram_batch_create(&dg, (KlDgramBatchDir)0, 4, 1500) == NULL);
    ASSERT_TRUE(kl_datagram_batch_create(&dg, KL_DGRAM_BATCH_SEND, 0, 1500) == NULL);
    ASSERT_TRUE(kl_datagram_batch_create(&dg, KL_DGRAM_BATCH_SEND, 4, 0) == NULL);
    ASSERT_TRUE(kl_datagram_batch_create(&dg, KL_DGRAM_BATCH_SEND, 4, SIZE_MAX) == NULL); /* overflow */

    /* Both create branches are exercised by whitebox-flipping the facade completion flag — the create
     * gate reads dg.completion only (no I/O), so this is backend-agnostic (works under a readiness OR a
     * completion build). READINESS branch: all three directions create; caps govern the provider block,
     * not success. */
    dg.completion = 0;
    KlDatagramBatch *br = kl_datagram_batch_create(&dg, KL_DGRAM_BATCH_RECV, 8, 2048);
    ASSERT_TRUE(br != NULL);
    ASSERT_EQ(&dg, br->owner);
    ASSERT_EQ((int)KL_DGRAM_BATCH_RECV, (int)br->dir);
    ASSERT_TRUE(br->rx_slots != NULL);                                   /* staging always allocated */
    ASSERT_EQ((pcaps & KL_DGRAM_CAP_RX_BATCH) != 0, br->rx_block != NULL);/* block iff supported */
    ASSERT_TRUE(br->gso_buf == NULL);                                    /* no send side */
    ASSERT_EQ(0, kl_datagram_batch_free(br));

    KlDatagramBatch *bs = kl_datagram_batch_create(&dg, KL_DGRAM_BATCH_SEND, 4, 1500);
    ASSERT_TRUE(bs != NULL);
    ASSERT_TRUE(bs->gso_buf != NULL);                                    /* GSO group buffer */
    ASSERT_EQ((pcaps & KL_DGRAM_CAP_TX_BATCH) != 0, bs->tx_block != NULL);
    ASSERT_TRUE(bs->rx_slots == NULL);                                   /* no recv side */
    ASSERT_EQ(0, kl_datagram_batch_free(bs));

    KlDatagramBatch *bb = kl_datagram_batch_create(&dg, KL_DGRAM_BATCH_BOTH, 4, 1500);
    ASSERT_TRUE(bb != NULL);
    ASSERT_TRUE(bb->rx_slots != NULL && bb->gso_buf != NULL);
    ASSERT_EQ(0, kl_datagram_batch_free(bb));

    ASSERT_EQ(0, kl_datagram_batch_free(NULL));   /* NULL is a no-op */

    /* COMPLETION branch (E2): RECV/BOTH refused, SEND allowed. */
    dg.completion = 1;
    ASSERT_TRUE(kl_datagram_batch_create(&dg, KL_DGRAM_BATCH_RECV, 4, 1500) == NULL);
    ASSERT_TRUE(kl_datagram_batch_create(&dg, KL_DGRAM_BATCH_BOTH, 4, 1500) == NULL);
    KlDatagramBatch *bsc = kl_datagram_batch_create(&dg, KL_DGRAM_BATCH_SEND, 4, 1500);
    ASSERT_TRUE(bsc != NULL);
    ASSERT_EQ(0, kl_datagram_batch_free(bsc));
    dg.completion = orig_completion;   /* restore the real backend's mode before teardown */

    ASSERT_EQ(0, kl_datagram_close_cancel(&dg));
    ASSERT_EQ(0, kl_datagram_free(&dg));
    kl_event_ctx_free(&ctx);
}

/* ── allocation-failure unwind at EVERY allocation (no leak) ────────────────────────────────────── */
UTEST(dgram_batch, create_alloc_failure_unwind) {
    CountAlloc c;
    KlAllocator a = ca_init(&c);
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &a));
    const KlSocketProvider *sp = ctx.sockets;
    KlSocketHandle fd = prep_fd(sp);
    ASSERT_TRUE(kl_handle_valid(fd));

    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig cfg = { .ctx = &ctx, .alloc = &a, .sockets = sp, .fd = fd,
                             .send_slots = 4, .send_slot_cap = 1500, .recv_cap = 2048 };
    ASSERT_EQ(0, kl_datagram_init_ex(&dg, &cfg, 0));

    /* For each of the create's allocations in turn, inject a failure and assert the outstanding-alloc
     * count returns to baseline (every prior allocation unwound). Once fail_at exceeds create's alloc
     * count, create succeeds and is freed — also back to baseline. */
    /* SEND is valid on both readiness and completion datagrams (E2), so this exercises create's
     * allocations regardless of the build's backend. */
    for (int k = 1; k <= 8; k++) {
        int base_live = c.live;
        c.calls = 0; c.fail_at = k;
        KlDatagramBatch *b = kl_datagram_batch_create(&dg, KL_DGRAM_BATCH_SEND, 4, 1500);
        c.fail_at = 0;
        if (b) ASSERT_EQ(0, kl_datagram_batch_free(b));
        ASSERT_EQ(base_live, c.live);
    }

    ASSERT_EQ(0, kl_datagram_close_cancel(&dg));
    ASSERT_EQ(0, kl_datagram_free(&dg));
    kl_event_ctx_free(&ctx);
}

/* ── partial provider batch vtables fall back to NULL (no new-without-free leak), complete allocate ── */
UTEST(dgram_batch, partial_vtable_falls_back_no_leak) {
    CountAlloc c;
    KlAllocator a = ca_init(&c);
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &a));
    const KlSocketProvider *sp = ctx.sockets;
    KlSocketHandle fd = prep_fd(sp);
    ASSERT_TRUE(kl_handle_valid(fd));

    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig cfg = { .ctx = &ctx, .alloc = &a, .sockets = sp, .fd = fd,
                             .send_slots = 4, .send_slot_cap = 1500, .recv_cap = 2048 };
    ASSERT_EQ(0, kl_datagram_init_ex(&dg, &cfg, 0));

    /* Whitebox-inject a mock provider + caps; the create gate reads dg.sockets->dgram + dg.provider_caps
     * + dg.completion only (no I/O). Restore before teardown. */
    const KlSocketProvider *save_sp = dg.sockets;
    unsigned save_caps = dg.provider_caps;
    int save_comp = dg.completion;
    KlSocketProvider mockp; memset(&mockp, 0, sizeof(mockp));
    dg.sockets = &mockp; dg.completion = 0;
    int base = c.live;

    /* RX new+recv_batch but NO free, cap bit SET → block must stay NULL (fallback), no leak. */
    mockp.dgram = &MOCK_RX_NOFREE; dg.provider_caps = KL_DGRAM_CAP_RX_BATCH;
    KlDatagramBatch *b = kl_datagram_batch_create(&dg, KL_DGRAM_BATCH_RECV, 4, 1500);
    ASSERT_TRUE(b != NULL);
    ASSERT_TRUE(b->rx_block == NULL);
    ASSERT_TRUE(b->rx_slots != NULL);
    ASSERT_EQ(0, kl_datagram_batch_free(b));
    ASSERT_EQ(base, c.live);

    /* TX new+free but NO send_batch, cap bit SET → block must stay NULL. */
    mockp.dgram = &MOCK_TX_NOSEND; dg.provider_caps = KL_DGRAM_CAP_TX_BATCH;
    b = kl_datagram_batch_create(&dg, KL_DGRAM_BATCH_SEND, 4, 1500);
    ASSERT_TRUE(b != NULL);
    ASSERT_TRUE(b->tx_block == NULL);
    ASSERT_TRUE(b->gso_buf != NULL);
    ASSERT_EQ(0, kl_datagram_batch_free(b));
    ASSERT_EQ(base, c.live);

    /* COMPLETE vtable → both blocks allocated (and freed on teardown). */
    mockp.dgram = &MOCK_COMPLETE; dg.provider_caps = KL_DGRAM_CAP_RX_BATCH | KL_DGRAM_CAP_TX_BATCH;
    b = kl_datagram_batch_create(&dg, KL_DGRAM_BATCH_BOTH, 4, 1500);
    ASSERT_TRUE(b != NULL);
    ASSERT_TRUE(b->rx_block != NULL && b->tx_block != NULL);
    ASSERT_EQ(0, kl_datagram_batch_free(b));
    ASSERT_EQ(base, c.live);

    dg.sockets = save_sp; dg.provider_caps = save_caps; dg.completion = save_comp;
    ASSERT_EQ(0, kl_datagram_close_cancel(&dg));
    ASSERT_EQ(0, kl_datagram_free(&dg));
    kl_event_ctx_free(&ctx);
}

/* ── allocation-failure unwind on a RECV/BOTH batch with a COMPLETE mock vtable (exercises
 *    rx_batch_new / rx_slots / tx_batch_new / gso_buf, incl. failure AFTER a provider block succeeds) ── */
UTEST(dgram_batch, create_alloc_failure_recv_both_mock) {
    CountAlloc c;
    KlAllocator a = ca_init(&c);
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &a));
    const KlSocketProvider *sp = ctx.sockets;
    KlSocketHandle fd = prep_fd(sp);
    ASSERT_TRUE(kl_handle_valid(fd));

    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig cfg = { .ctx = &ctx, .alloc = &a, .sockets = sp, .fd = fd,
                             .send_slots = 4, .send_slot_cap = 1500, .recv_cap = 2048 };
    ASSERT_EQ(0, kl_datagram_init_ex(&dg, &cfg, 0));

    const KlSocketProvider *save_sp = dg.sockets;
    unsigned save_caps = dg.provider_caps;
    int save_comp = dg.completion;
    KlSocketProvider mockp; memset(&mockp, 0, sizeof(mockp));
    mockp.dgram = &MOCK_COMPLETE;
    dg.sockets = &mockp; dg.completion = 0;
    dg.provider_caps = KL_DGRAM_CAP_RX_BATCH | KL_DGRAM_CAP_TX_BATCH;

    /* BOTH create over the complete mock allocates: struct, rx_block(mock new), rx_slots, tx_block(mock
     * new), gso_buf — so injecting a failure at each in turn exercises the recv side, the tx side, and
     * the "provider block succeeded then a LATER allocation failed → the block is freed" unwind. */
    for (int k = 1; k <= 8; k++) {
        int base_live = c.live;
        c.calls = 0; c.fail_at = k;
        KlDatagramBatch *b = kl_datagram_batch_create(&dg, KL_DGRAM_BATCH_BOTH, 4, 1500);
        c.fail_at = 0;
        if (b) ASSERT_EQ(0, kl_datagram_batch_free(b));
        ASSERT_EQ(base_live, c.live);
    }

    dg.sockets = save_sp; dg.provider_caps = save_caps; dg.completion = save_comp;
    ASSERT_EQ(0, kl_datagram_close_cancel(&dg));
    ASSERT_EQ(0, kl_datagram_free(&dg));
    kl_event_ctx_free(&ctx);
}

/* ── a non-IP datagram fd reports NONE of the M5 support bits (exact-fd family rule) ────────────── */
UTEST(dgram_batch, non_ip_fd_reports_no_m5_caps) {
#if defined(AF_UNIX)
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return;   /* AF_UNIX/SOCK_DGRAM not supported here — skip */
    const KlDatagramOps *ops = kl_sockdef_dgram();
    unsigned caps = ops->caps(NULL, (KlSocketHandle)fd);
    const unsigned m5 = KL_DGRAM_CAP_RX_BATCH | KL_DGRAM_CAP_TX_BATCH |
                        KL_DGRAM_CAP_GSO | KL_DGRAM_CAP_GRO;
    ASSERT_EQ(0u, (caps & m5));   /* a non-IP fd must never be told it has UDP batch/GSO/GRO */
    close(fd);
#endif
}

/* ── kl_datagram_batch_free is refused while gso_busy; succeeds once cleared ─────────────────────── */
UTEST(dgram_batch, gso_busy_refuses_free) {
    CountAlloc c;
    KlAllocator a = ca_init(&c);
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &a));
    const KlSocketProvider *sp = ctx.sockets;
    KlSocketHandle fd = prep_fd(sp);
    ASSERT_TRUE(kl_handle_valid(fd));

    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig cfg = { .ctx = &ctx, .alloc = &a, .sockets = sp, .fd = fd,
                             .send_slots = 4, .send_slot_cap = 1500, .recv_cap = 2048 };
    ASSERT_EQ(0, kl_datagram_init_ex(&dg, &cfg, 0));

    int base = c.live;
    KlDatagramBatch *b = kl_datagram_batch_create(&dg, KL_DGRAM_BATCH_SEND, 4, 1500);
    ASSERT_TRUE(b != NULL);
    int held = c.live;
    ASSERT_TRUE(held > base);                       /* the batch is allocated */

    b->gso_busy = 1;                                /* whitebox: a GSO group is in flight (M5.2 sets this) */
    ASSERT_EQ(-1, kl_datagram_batch_free(b));       /* refused — the group still reads the buffer */
    ASSERT_EQ(held, c.live);                         /* nothing freed */

    b->gso_busy = 0;                                /* the group retired */
    ASSERT_EQ(0, kl_datagram_batch_free(b));        /* now reclaimable */
    ASSERT_EQ(base, c.live);                         /* fully freed */

    ASSERT_EQ(0, kl_datagram_close_cancel(&dg));
    ASSERT_EQ(0, kl_datagram_free(&dg));
    kl_event_ctx_free(&ctx);
}

/* ── M5.2a send batch: over a real loopback tx datagram + a raw rx peer ─────────────────────────── */

/* A bound raw rx UDP socket on 127.0.0.1; returns its port (0 = fail). */
static int mk_rx(int *fd_out) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(0x7f000001);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { close(fd); return 0; }
    socklen_t sl = sizeof(a);
    if (getsockname(fd, (struct sockaddr *)&a, &sl) != 0) { close(fd); return 0; }
    int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    *fd_out = fd;
    return (int)ntohs(a.sin_port);
}
/* Pump the loop until a datagram's close completes (a completion in-flight send retires asynchronously
 * through the cancel; readiness is synchronous — the loop just no-ops). */
static void pump_close(KlEventCtx *ctx, KlDatagram *dg) {
    for (int i = 0; i < 200 && kl_datagram_close_state(dg) != KL_DGRAM_CLOSE_CLOSED; i++)
        kl_event_ctx_run(ctx, 8, 10);
}
/* Drain all pending datagrams from a nonblocking raw fd; returns the count, sums the bytes. */
static int raw_drain(int fd, size_t *total) {
    unsigned char buf[2048]; int n = 0; *total = 0;
    for (;;) {
        ssize_t r = recv(fd, buf, sizeof(buf), 0);
        if (r < 0) break;
        n++; *total += (size_t)r;
    }
    return n;
}

/* All N accepted + delivered (fast sendmmsg where supported, else the portable loop). */
UTEST(dgram_batch, send_batch_all_delivered) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &a));
    const KlSocketProvider *sp = ctx.sockets;
    int rxfd, port = mk_rx(&rxfd);
    ASSERT_NE(0, port);

    KlSocketHandle txfd = prep_fd(sp);
    ASSERT_TRUE(kl_handle_valid(txfd));
    KlDatagram tx; memset(&tx, 0, sizeof(tx));
    KlDatagramConfig cfg = { .ctx = &ctx, .alloc = &a, .sockets = sp, .fd = txfd,
                             .send_slots = 8, .send_slot_cap = 1500, .recv_cap = 2048 };
    ASSERT_EQ(0, kl_datagram_init_ex(&tx, &cfg, 0));
    KlDatagramBatch *b = kl_datagram_batch_create(&tx, KL_DGRAM_BATCH_SEND, 8, 1500);
    ASSERT_TRUE(b != NULL);

    KlSockAddr dest; kl_sockaddr_parse(&dest, "127.0.0.1", (uint16_t)port);
    KlDgramTxDesc descs[3];
    const char *msgs[3] = { "aa", "bbbb", "cccccc" };
    for (int i = 0; i < 3; i++) {
        memset(&descs[i], 0, sizeof(descs[i]));
        descs[i].data = msgs[i]; descs[i].len = strlen(msgs[i]); descs[i].dest = dest; descs[i].tos = -1;
        /* src stays zeroed (memset) → AF_UNSPEC → no source-pin */
    }
    KlDatagramSendStatus stop = KL_DATAGRAM_ERROR;
    int acc = kl_datagram_send_batch(&tx, b, descs, 3, &stop);
    ASSERT_EQ(3, acc);
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)stop);

    size_t total = 0; int got = 0;
    for (int i = 0; i < 50 && got < 3; i++) { kl_event_ctx_run(&ctx, 8, 10); size_t t; got += raw_drain(rxfd, &t); total += t; }
    ASSERT_EQ(3, got);
    ASSERT_EQ((size_t)(2 + 4 + 6), total);

    ASSERT_EQ(0, kl_datagram_batch_free(b));
    close(rxfd);
    ASSERT_EQ(0, kl_datagram_close_cancel(&tx)); pump_close(&ctx, &tx); ASSERT_EQ(0, kl_datagram_free(&tx));
    kl_event_ctx_free(&ctx);
}

/* Partial accept: n > capacity → accepted prefix + stop = WOULD_BLOCK; the remainder is retained. */
UTEST(dgram_batch, send_batch_partial_accept) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &a));
    const KlSocketProvider *sp = ctx.sockets;
    int rxfd, port = mk_rx(&rxfd);
    ASSERT_NE(0, port);

    KlSocketHandle txfd = prep_fd(sp);
    KlDatagram tx; memset(&tx, 0, sizeof(tx));
    KlDatagramConfig cfg = { .ctx = &ctx, .alloc = &a, .sockets = sp, .fd = txfd,
                             .send_slots = 2, .send_slot_cap = 1500, .recv_cap = 2048 };
    ASSERT_EQ(0, kl_datagram_init_ex(&tx, &cfg, 0));
    KlDatagramBatch *b = kl_datagram_batch_create(&tx, KL_DGRAM_BATCH_SEND, 2, 1500);
    ASSERT_TRUE(b != NULL);

    KlSockAddr dest; kl_sockaddr_parse(&dest, "127.0.0.1", (uint16_t)port);
    KlDgramTxDesc descs[4];
    for (int i = 0; i < 4; i++) {
        memset(&descs[i], 0, sizeof(descs[i]));
        descs[i].data = "x"; descs[i].len = 1; descs[i].dest = dest; descs[i].tos = -1;
    }
    /* Only the batch's slot capacity (2) is ADMITTED per call — the 3rd enqueue hits the slot cap →
     * WOULD_BLOCK. Send the remaining descriptors over successive rounds, pumping between them so the
     * admitted datagrams drain and free their slots. Backend-adaptive (readiness drains synchronously;
     * completion drains via the single-flight pump). */
    int total = 0;
    for (int round = 0; round < 10 && total < 4; round++) {
        KlDatagramSendStatus stop = KL_DATAGRAM_ACCEPTED;
        int acc = kl_datagram_send_batch(&tx, b, descs + total, 4 - total, &stop);
        if (round == 0 && !tx.completion) {       /* readiness: the first call admits exactly the 2 slots */
            ASSERT_EQ(2, acc);
            ASSERT_EQ((int)KL_DATAGRAM_WOULD_BLOCK, (int)stop);
        }
        total += acc;
        kl_event_ctx_run(&ctx, 8, 10);            /* let the admitted prefix drain (frees slots) */
    }
    ASSERT_EQ(4, total);                          /* all four eventually admitted */

    int got = 0;
    for (int i = 0; i < 50 && got < 4; i++) { kl_event_ctx_run(&ctx, 8, 10); size_t t; got += raw_drain(rxfd, &t); }
    ASSERT_EQ(4, got);

    ASSERT_EQ(0, kl_datagram_batch_free(b));
    close(rxfd);
    ASSERT_EQ(0, kl_datagram_close_cancel(&tx)); pump_close(&ctx, &tx); ASSERT_EQ(0, kl_datagram_free(&tx));
    kl_event_ctx_free(&ctx);
}

/* TOO_LARGE stops acceptance at that index; owner/direction mismatches are refused. */
UTEST(dgram_batch, send_batch_too_large_and_validation) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &a));
    const KlSocketProvider *sp = ctx.sockets;

    KlSocketHandle txfd = prep_fd(sp);
    KlDatagram tx; memset(&tx, 0, sizeof(tx));
    KlDatagramConfig cfg = { .ctx = &ctx, .alloc = &a, .sockets = sp, .fd = txfd,
                             .send_slots = 8, .send_slot_cap = 4, .recv_cap = 2048 };   /* tiny slot cap */
    ASSERT_EQ(0, kl_datagram_init_ex(&tx, &cfg, 0));
    KlDatagramBatch *bs = kl_datagram_batch_create(&tx, KL_DGRAM_BATCH_SEND, 8, 4);
    ASSERT_TRUE(bs != NULL);

    KlSockAddr dest; kl_sockaddr_parse(&dest, "127.0.0.1", 9999);
    KlDgramTxDesc descs[3];
    memset(descs, 0, sizeof(descs));
    descs[0].data = "ok"; descs[0].len = 2; descs[0].dest = dest; descs[0].tos = -1;
    descs[1].data = "toolong"; descs[1].len = 7; descs[1].dest = dest; descs[1].tos = -1;   /* > slot cap 4 */
    descs[2].data = "ok"; descs[2].len = 2; descs[2].dest = dest; descs[2].tos = -1;
    KlDatagramSendStatus stop = KL_DATAGRAM_ACCEPTED;
    int acc = kl_datagram_send_batch(&tx, bs, descs, 3, &stop);
    ASSERT_EQ(1, acc);                                  /* only descs[0] admitted */
    ASSERT_EQ((int)KL_DATAGRAM_TOO_LARGE, (int)stop);   /* descs[1] is permanently too large */

    /* validation (backend-independent whitebox flips — a RECV batch cannot exist on a completion
     * datagram, E2): a non-SEND direction and a foreign owner are each refused with -1. */
    KlDgramBatchDir save_dir = bs->dir;
    bs->dir = KL_DGRAM_BATCH_RECV;
    stop = KL_DATAGRAM_ACCEPTED;
    ASSERT_EQ(-1, kl_datagram_send_batch(&tx, bs, descs, 1, &stop));
    ASSERT_EQ((int)KL_DATAGRAM_ERROR, (int)stop);
    bs->dir = save_dir;

    KlDatagram *save_owner = bs->owner;
    bs->owner = NULL;                                   /* not owned by tx */
    stop = KL_DATAGRAM_ACCEPTED;
    ASSERT_EQ(-1, kl_datagram_send_batch(&tx, bs, descs, 1, &stop));
    ASSERT_EQ((int)KL_DATAGRAM_ERROR, (int)stop);
    bs->owner = save_owner;

    ASSERT_EQ(0, kl_datagram_batch_free(bs));
    ASSERT_EQ(0, kl_datagram_close_cancel(&tx)); pump_close(&ctx, &tx); ASSERT_EQ(0, kl_datagram_free(&tx));
    kl_event_ctx_free(&ctx);
}

/* ── a gating provider that forces a deterministic HARD send error for a marked datagram ────────── */
static kl_ssize_t (*g_real_send)(void *, KlSocketHandle, const void *, size_t,
                                 const KlSockAddr *, const KlSockAddr *, int);
static KlDatagramOps    g_gate_dg;
static KlSocketProvider g_gate_sp;
static int g_block;   /* > 0: the next g_block send attempts WOULD_BLOCK (EAGAIN), then decrement */
static kl_ssize_t gate_send(void *ctx, KlSocketHandle fd, const void *data, size_t len,
                            const KlSockAddr *dest, const KlSockAddr *src, int tos) {
    if (g_block > 0) { g_block--; errno = EAGAIN; return -1; }                       /* forced backpressure */
    if (len > 0 && ((const char *)data)[0] == 'B') { errno = EACCES; return -1; }   /* deterministic hard error */
    return g_real_send(ctx, fd, data, len, dest, src, tos);
}
/* Wraps the built-in POSIX provider (real fds), overrides send, and nulls the tx-batch ops so the send
 * path always takes the portable single-send loop (so the gate intercepts every datagram). */
static const KlSocketProvider *gating_provider(void) {
    g_gate_sp = *kl_socket_provider_posix();
    g_gate_dg = *g_gate_sp.dgram;
    g_real_send = g_gate_dg.send;
    g_gate_dg.send = gate_send;
    g_gate_dg.send_batch = NULL; g_gate_dg.tx_batch_new = NULL; g_gate_dg.tx_batch_free = NULL;
    g_gate_dg.send_gso = NULL;   /* force GSO → UNSUPPORTED → per-segment fallback through gate_send */
    g_gate_sp.dgram = &g_gate_dg;
    return &g_gate_sp;
}

/* Recoverable per-datagram error (facade), bad in the MIDDLE — the hard part (P1-2): after the good
 * prefix drains, the retained bad-middle datagram reaches the head on a later writable edge and is
 * dropped there via the ordinary single-flight path (its recoverable slot provenance), NOT poisoned
 * with the sticky error. It surfaces via last_error; the good datagrams still deliver; the call does
 * not fail. Readiness-only: the gating provider is native-fd, incompatible with a completion loop. */
UTEST(dgram_batch, send_batch_recoverable_drop) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &a));
    if (kl_event_caps(&ctx.loop) & KL_EVENT_CAP_COMPLETION) { kl_event_ctx_free(&ctx); return; }  /* readiness only */

    const KlSocketProvider *sp = gating_provider();
    g_block = 0;
    int rxfd, port = mk_rx(&rxfd);
    ASSERT_NE(0, port);
    KlSocketHandle txfd = prep_fd(sp);
    ASSERT_TRUE(kl_handle_valid(txfd));

    KlDatagram tx; memset(&tx, 0, sizeof(tx));
    KlDatagramConfig cfg = { .ctx = &ctx, .alloc = &a, .sockets = sp, .fd = txfd,
                             .send_slots = 8, .send_slot_cap = 1500, .recv_cap = 2048 };
    ASSERT_EQ(0, kl_datagram_init_ex(&tx, &cfg, 0));
    KlDatagramBatch *b = kl_datagram_batch_create(&tx, KL_DGRAM_BATCH_SEND, 8, 1500);
    ASSERT_TRUE(b != NULL);

    /* The marked datagram is in the MIDDLE: flush_batch sends the good prefix (short SENT), retains
     * [Bad, Ccc] and arms WRITE; the writable edge drains them single-flight, where Bad (recoverable)
     * hard-errors at the head and is dropped, then Ccc sends. */
    KlSockAddr dest; kl_sockaddr_parse(&dest, "127.0.0.1", (uint16_t)port);
    KlDgramTxDesc descs[3];
    memset(descs, 0, sizeof(descs));
    descs[0].data = "Aaa"; descs[0].len = 3; descs[0].dest = dest; descs[0].tos = -1;   /* delivers */
    descs[1].data = "Bad"; descs[1].len = 3; descs[1].dest = dest; descs[1].tos = -1;   /* gate → hard error */
    descs[2].data = "Ccc"; descs[2].len = 3; descs[2].dest = dest; descs[2].tos = -1;   /* delivers */

    KlDatagramSendStatus stop = KL_DATAGRAM_ERROR;
    int acc = kl_datagram_send_batch(&tx, b, descs, 3, &stop);
    ASSERT_EQ(3, acc);                                  /* all three ADMITTED (the drop happens on drain) */
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)stop);

    int got = 0;
    for (int i = 0; i < 50 && got < 2; i++) { kl_event_ctx_run(&ctx, 8, 10); size_t t; got += raw_drain(rxfd, &t); }
    ASSERT_EQ(2, got);                                  /* the two good datagrams delivered; the marked one dropped */
    ASSERT_NE((int)KL_ERR_NONE, (int)kl_datagram_last_error(&tx));   /* the mid-queue drop surfaced (on_drop) */

    ASSERT_EQ(0, kl_datagram_batch_free(b));
    close(rxfd);
    ASSERT_EQ(0, kl_datagram_close_cancel(&tx)); pump_close(&ctx, &tx); ASSERT_EQ(0, kl_datagram_free(&tx));
    kl_event_ctx_free(&ctx);
}

/* Backpressure: the batch drain WOULD_BLOCKs and retains the remainder, which arms WRITE and drains on
 * the ordinary writable edge (single-flight) — nothing is lost. Readiness-only (gating provider). */
UTEST(dgram_batch, send_batch_backpressure_drains_on_writable) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &a));
    if (kl_event_caps(&ctx.loop) & KL_EVENT_CAP_COMPLETION) { kl_event_ctx_free(&ctx); return; }

    const KlSocketProvider *sp = gating_provider();
    int rxfd, port = mk_rx(&rxfd);
    ASSERT_NE(0, port);
    KlSocketHandle txfd = prep_fd(sp);
    ASSERT_TRUE(kl_handle_valid(txfd));

    KlDatagram tx; memset(&tx, 0, sizeof(tx));
    KlDatagramConfig cfg = { .ctx = &ctx, .alloc = &a, .sockets = sp, .fd = txfd,
                             .send_slots = 8, .send_slot_cap = 1500, .recv_cap = 2048 };
    ASSERT_EQ(0, kl_datagram_init_ex(&tx, &cfg, 0));
    KlDatagramBatch *b = kl_datagram_batch_create(&tx, KL_DGRAM_BATCH_SEND, 8, 1500);
    ASSERT_TRUE(b != NULL);

    g_block = 3;   /* the first three send attempts WOULD_BLOCK → the whole run is retained */
    KlSockAddr dest; kl_sockaddr_parse(&dest, "127.0.0.1", (uint16_t)port);
    KlDgramTxDesc descs[4];
    for (int i = 0; i < 4; i++) {
        memset(&descs[i], 0, sizeof(descs[i]));
        descs[i].data = "yo"; descs[i].len = 2; descs[i].dest = dest; descs[i].tos = -1;
    }
    KlDatagramSendStatus stop = KL_DATAGRAM_ERROR;
    int acc = kl_datagram_send_batch(&tx, b, descs, 4, &stop);
    ASSERT_EQ(4, acc);                                   /* all admitted */
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)stop);
    ASSERT_TRUE(kl_dgram_send_queued(&tx.core->send) > 0);  /* the drain WOULD_BLOCKed → remainder retained */

    int got = 0;
    for (int i = 0; i < 100 && got < 4; i++) { kl_event_ctx_run(&ctx, 8, 10); size_t t; got += raw_drain(rxfd, &t); }
    ASSERT_EQ(4, got);                                   /* all four eventually delivered via writable edges */
    ASSERT_EQ((size_t)0, kl_dgram_send_queued(&tx.core->send));

    ASSERT_EQ(0, kl_datagram_batch_free(b));
    close(rxfd);
    ASSERT_EQ(0, kl_datagram_close_cancel(&tx)); pump_close(&ctx, &tx); ASSERT_EQ(0, kl_datagram_free(&tx));
    kl_event_ctx_free(&ctx);
}

/* P1-1: a teardown from a drain callback DURING kl_datagram_send_batch must not UAF — the facade
 * brackets the whole operation (dispatch_begin/end), so the drain's activity release cannot free
 * dg/core until dispatch_end, after which only the local `accepted` is touched. Readiness-only (the
 * batch drain — and thus on_drain — is synchronous within the call). */
static int g_torn;
static void batch_teardown_reclaim(void *ctx) { (void)ctx; g_torn++; }
static void batch_on_drain_teardown(void *ud) {
    kl_datagram_teardown((KlDatagram *)ud, batch_teardown_reclaim, NULL);
}
UTEST(dgram_batch, send_batch_teardown_from_drain_no_uaf) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &a));
    if (kl_event_caps(&ctx.loop) & KL_EVENT_CAP_COMPLETION) { kl_event_ctx_free(&ctx); return; }
    const KlSocketProvider *sp = ctx.sockets;

    KlSocketHandle txfd = prep_fd(sp);
    ASSERT_TRUE(kl_handle_valid(txfd));
    KlDatagram tx; memset(&tx, 0, sizeof(tx));
    KlDatagramConfig cfg = { .ctx = &ctx, .alloc = &a, .sockets = sp, .fd = txfd,
                             .send_slots = 4, .send_slot_cap = 1500, .recv_cap = 2048 };
    ASSERT_EQ(0, kl_datagram_init_ex(&tx, &cfg, 0));
    KlDatagramBatch *b = kl_datagram_batch_create(&tx, KL_DGRAM_BATCH_SEND, 4, 1500);
    ASSERT_TRUE(b != NULL);
    kl_datagram_on_drain(&tx, batch_on_drain_teardown, &tx);   /* the drain tears the datagram down */
    g_torn = 0;

    KlSockAddr dest; kl_sockaddr_parse(&dest, "127.0.0.1", 9999);
    KlDgramTxDesc d; memset(&d, 0, sizeof(d));
    d.data = "z"; d.len = 1; d.dest = dest; d.tos = -1;
    /* one datagram → drains synchronously → count 0 → on_drain fires (INSIDE the batch call) → teardown,
     * deferred by the facade bracket to dispatch_end. No UAF (ASan) on the post-drain reconcile. */
    KlDatagramSendStatus stop = KL_DATAGRAM_ERROR;
    int acc = kl_datagram_send_batch(&tx, b, &d, 1, &stop);
    ASSERT_EQ(1, acc);
    ASSERT_EQ(1, g_torn);                          /* reclaimed at dispatch_end (deferred, once) */

    ASSERT_EQ(0, kl_datagram_batch_free(b));        /* the SEND batch is caller-owned (tx already reclaimed) */
    kl_event_ctx_free(&ctx);
}

/* ── M5.2b GSO ──────────────────────────────────────────────────────────────────────────────────── */

/* send_gso delivers the whole payload: one send_gso syscall where supported, else the same segments
 * per-send. Robust across both (assert total bytes; the segment count where the platform falls back). */
UTEST(dgram_batch, gso_delivers) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &a));
    const KlSocketProvider *sp = ctx.sockets;
    int rxfd, port = mk_rx(&rxfd);
    ASSERT_NE(0, port);
    KlSocketHandle txfd = prep_fd(sp);
    ASSERT_TRUE(kl_handle_valid(txfd));
    KlDatagram tx; memset(&tx, 0, sizeof(tx));
    KlDatagramConfig cfg = { .ctx = &ctx, .alloc = &a, .sockets = sp, .fd = txfd,
                             .send_slots = 8, .send_slot_cap = 2048, .recv_cap = 2048 };
    ASSERT_EQ(0, kl_datagram_init_ex(&tx, &cfg, 0));
    KlDatagramBatch *b = kl_datagram_batch_create(&tx, KL_DGRAM_BATCH_SEND, 8, 2048);
    ASSERT_TRUE(b != NULL);
    int gso_cap = (kl_datagram_provider_caps(&tx) & KL_DGRAM_CAP_GSO) != 0;

    KlSockAddr dest; kl_sockaddr_parse(&dest, "127.0.0.1", (uint16_t)port);
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED,
              (int)kl_datagram_send_gso(&tx, b, "112233", 6, 2, &dest));   /* 3 segments of 2 bytes */

    size_t total = 0; int got = 0;
    for (int i = 0; i < 80 && total < 6; i++) { kl_event_ctx_run(&ctx, 8, 10); size_t t; got += raw_drain(rxfd, &t); total += t; }
    ASSERT_EQ((size_t)6, total);                  /* the whole payload egressed */
    if (!gso_cap) ASSERT_EQ(3, got);              /* per-segment fallback → exactly nseg datagrams */

    /* the group retired → gso_busy cleared → the batch is freeable */
    for (int i = 0; i < 20 && b->gso_busy; i++) kl_event_ctx_run(&ctx, 8, 10);
    ASSERT_EQ(0, b->gso_busy);
    ASSERT_EQ(0, kl_datagram_batch_free(b));
    close(rxfd);
    ASSERT_EQ(0, kl_datagram_close_cancel(&tx)); pump_close(&ctx, &tx); ASSERT_EQ(0, kl_datagram_free(&tx));
    kl_event_ctx_free(&ctx);
}

/* Validation + bounds: bad args → ERROR; over-cap → TOO_LARGE. */
UTEST(dgram_batch, gso_validation_and_bounds) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &a));
    const KlSocketProvider *sp = ctx.sockets;
    KlSocketHandle txfd = prep_fd(sp);
    KlDatagram tx; memset(&tx, 0, sizeof(tx));
    KlDatagramConfig cfg = { .ctx = &ctx, .alloc = &a, .sockets = sp, .fd = txfd,
                             .send_slots = 4, .send_slot_cap = 100, .recv_cap = 2048 };
    ASSERT_EQ(0, kl_datagram_init_ex(&tx, &cfg, 0));
    KlDatagramBatch *bs = kl_datagram_batch_create(&tx, KL_DGRAM_BATCH_SEND, 4, 100);   /* group buf = 400 */
    ASSERT_TRUE(bs != NULL);
    KlSockAddr dest; kl_sockaddr_parse(&dest, "127.0.0.1", 9999);

    ASSERT_EQ((int)KL_DATAGRAM_ERROR, (int)kl_datagram_send_gso(&tx, bs, "x", 1, 0, &dest));   /* seg 0 */
    ASSERT_EQ((int)KL_DATAGRAM_ERROR, (int)kl_datagram_send_gso(&tx, bs, NULL, 4, 2, &dest));  /* NULL buf */
    ASSERT_EQ((int)KL_DATAGRAM_TOO_LARGE, (int)kl_datagram_send_gso(&tx, bs, "buf", 3, 200, &dest)); /* seg>cap */
    { char big[500] = {0};
      ASSERT_EQ((int)KL_DATAGRAM_TOO_LARGE, (int)kl_datagram_send_gso(&tx, bs, big, 500, 50, &dest)); } /* >buf */

    /* wrong owner / non-SEND direction → ERROR (whitebox flips, backend-agnostic) */
    void *save = bs->owner; bs->owner = NULL;
    ASSERT_EQ((int)KL_DATAGRAM_ERROR, (int)kl_datagram_send_gso(&tx, bs, "ab", 2, 2, &dest));
    bs->owner = save;
    KlDgramBatchDir sd = bs->dir; bs->dir = KL_DGRAM_BATCH_RECV;
    ASSERT_EQ((int)KL_DATAGRAM_ERROR, (int)kl_datagram_send_gso(&tx, bs, "ab", 2, 2, &dest));
    bs->dir = sd;

    ASSERT_EQ(0, kl_datagram_batch_free(bs));
    ASSERT_EQ(0, kl_datagram_close_cancel(&tx)); pump_close(&ctx, &tx); ASSERT_EQ(0, kl_datagram_free(&tx));
    kl_event_ctx_free(&ctx);
}

/* One group per batch + gso_busy lifetime: a retained group makes a 2nd send_gso WOULD_BLOCK and
 * batch_free -1; draining clears gso_busy and the batch frees. Readiness-only (gating provider). */
UTEST(dgram_batch, gso_busy_backpressure_and_free) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &a));
    if (kl_event_caps(&ctx.loop) & KL_EVENT_CAP_COMPLETION) { kl_event_ctx_free(&ctx); return; }
    const KlSocketProvider *sp = gating_provider();
    int rxfd, port = mk_rx(&rxfd);
    ASSERT_NE(0, port);
    KlSocketHandle txfd = prep_fd(sp);
    KlDatagram tx; memset(&tx, 0, sizeof(tx));
    KlDatagramConfig cfg = { .ctx = &ctx, .alloc = &a, .sockets = sp, .fd = txfd,
                             .send_slots = 8, .send_slot_cap = 2048, .recv_cap = 2048 };
    ASSERT_EQ(0, kl_datagram_init_ex(&tx, &cfg, 0));
    KlDatagramBatch *b = kl_datagram_batch_create(&tx, KL_DGRAM_BATCH_SEND, 8, 2048);
    ASSERT_TRUE(b != NULL);

    g_block = 100;   /* every send WOULD_BLOCKs → the group is retained (gso fallback via gate_send) */
    KlSockAddr dest; kl_sockaddr_parse(&dest, "127.0.0.1", (uint16_t)port);
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send_gso(&tx, b, "112233", 6, 2, &dest));
    ASSERT_EQ(1, b->gso_busy);                                          /* group queued (buffer in use) */
    ASSERT_EQ((int)KL_DATAGRAM_WOULD_BLOCK, (int)kl_datagram_send_gso(&tx, b, "DD", 2, 2, &dest)); /* 2nd */
    ASSERT_EQ(-1, kl_datagram_batch_free(b));                          /* refused while gso_busy */

    g_block = 0;                                                       /* unblock → the group drains */
    int got = 0;
    for (int i = 0; i < 80 && got < 3; i++) { kl_event_ctx_run(&ctx, 8, 10); size_t t; got += raw_drain(rxfd, &t); }
    ASSERT_EQ(3, got);                                                 /* the 3 fallback segments delivered */
    ASSERT_EQ(0, b->gso_busy);                                         /* group retired → buffer free */
    ASSERT_EQ(0, kl_datagram_batch_free(b));                          /* now freeable */
    close(rxfd);
    ASSERT_EQ(0, kl_datagram_close_cancel(&tx)); pump_close(&ctx, &tx); ASSERT_EQ(0, kl_datagram_free(&tx));
    kl_event_ctx_free(&ctx);
}

/* close/discard releases a queued group and clears gso_busy (so the caller-owned batch frees). */
UTEST(dgram_batch, gso_close_clears_busy) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &a));
    if (kl_event_caps(&ctx.loop) & KL_EVENT_CAP_COMPLETION) { kl_event_ctx_free(&ctx); return; }
    const KlSocketProvider *sp = gating_provider();
    KlSocketHandle txfd = prep_fd(sp);
    KlDatagram tx; memset(&tx, 0, sizeof(tx));
    KlDatagramConfig cfg = { .ctx = &ctx, .alloc = &a, .sockets = sp, .fd = txfd,
                             .send_slots = 8, .send_slot_cap = 2048, .recv_cap = 2048 };
    ASSERT_EQ(0, kl_datagram_init_ex(&tx, &cfg, 0));
    KlDatagramBatch *b = kl_datagram_batch_create(&tx, KL_DGRAM_BATCH_SEND, 8, 2048);
    ASSERT_TRUE(b != NULL);

    g_block = 100;
    KlSockAddr dest; kl_sockaddr_parse(&dest, "127.0.0.1", 9999);
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send_gso(&tx, b, "112233", 6, 2, &dest));
    ASSERT_EQ(1, b->gso_busy);

    ASSERT_EQ(0, kl_datagram_close_cancel(&tx));    /* abortive close → discard the queued group */
    pump_close(&ctx, &tx);
    ASSERT_EQ(0, b->gso_busy);                       /* discard cleared gso_busy */
    ASSERT_EQ(0, kl_datagram_batch_free(b));         /* the caller-owned batch is now freeable */
    ASSERT_EQ(0, kl_datagram_free(&tx));
    g_block = 0;
    kl_event_ctx_free(&ctx);
}

/* Zero-length GSO: total_len 0 (buf may be NULL) → one empty datagram, delivered; no NULL+0 UB. */
UTEST(dgram_batch, gso_zero_length) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &a));
    const KlSocketProvider *sp = ctx.sockets;
    int rxfd, port = mk_rx(&rxfd);
    ASSERT_NE(0, port);
    KlSocketHandle txfd = prep_fd(sp);
    KlDatagram tx; memset(&tx, 0, sizeof(tx));
    KlDatagramConfig cfg = { .ctx = &ctx, .alloc = &a, .sockets = sp, .fd = txfd,
                             .send_slots = 4, .send_slot_cap = 64, .recv_cap = 64 };
    ASSERT_EQ(0, kl_datagram_init_ex(&tx, &cfg, 0));
    KlDatagramBatch *b = kl_datagram_batch_create(&tx, KL_DGRAM_BATCH_SEND, 4, 64);
    ASSERT_TRUE(b != NULL);

    KlSockAddr dest; kl_sockaddr_parse(&dest, "127.0.0.1", (uint16_t)port);
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send_gso(&tx, b, NULL, 0, 1, &dest));  /* empty, NULL buf */

    int got = 0; size_t total = 99;
    for (int i = 0; i < 60 && got < 1; i++) { kl_event_ctx_run(&ctx, 8, 10); got += raw_drain(rxfd, &total); }
    ASSERT_EQ(1, got);                            /* one empty datagram delivered */
    ASSERT_EQ((size_t)0, total);                  /* zero bytes */
    for (int i = 0; i < 20 && b->gso_busy; i++) kl_event_ctx_run(&ctx, 8, 10);
    ASSERT_EQ(0, b->gso_busy);

    ASSERT_EQ(0, kl_datagram_batch_free(b));
    close(rxfd);
    ASSERT_EQ(0, kl_datagram_close_cancel(&tx)); pump_close(&ctx, &tx); ASSERT_EQ(0, kl_datagram_free(&tx));
    kl_event_ctx_free(&ctx);
}

/* gso_active honestly reports the one-syscall fast path: false on a provider without CAP_GSO/send_gso
 * (whitebox-forced) and after the latch; true only on readiness + CAP_GSO + a real op + unset latch. */
UTEST(dgram_batch, gso_active_contract) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &a));
    const KlSocketProvider *sp = ctx.sockets;
    KlSocketHandle txfd = prep_fd(sp);
    KlDatagram tx; memset(&tx, 0, sizeof(tx));
    KlDatagramConfig cfg = { .ctx = &ctx, .alloc = &a, .sockets = sp, .fd = txfd,
                             .send_slots = 4, .send_slot_cap = 64, .recv_cap = 64 };
    ASSERT_EQ(0, kl_datagram_init_ex(&tx, &cfg, 0));

    int expect = !tx.completion && (kl_datagram_provider_caps(&tx) & KL_DGRAM_CAP_GSO) &&
                 ((sp && sp->dgram ? sp->dgram : kl_sockdef_dgram())->send_gso != NULL);
    ASSERT_EQ(expect, kl_datagram_gso_active(&tx));   /* honest per backend/provider */
    /* the latch forces it false regardless */
    tx.core->gso_unsupported = 1;
    ASSERT_EQ(0, kl_datagram_gso_active(&tx));
    tx.core->gso_unsupported = 0;
    /* a completion datagram never advertises the one-syscall path */
    int save = tx.completion; tx.completion = 1;
    ASSERT_EQ(0, kl_datagram_gso_active(&tx));
    tx.completion = save;

    ASSERT_EQ(0, kl_datagram_close_cancel(&tx)); pump_close(&ctx, &tx); ASSERT_EQ(0, kl_datagram_free(&tx));
    kl_event_ctx_free(&ctx);
}

/* Completion close/cancel with an in-flight fallback GSO segment: on a completion datagram GSO is
 * FALLBACK (per-segment single-flight). Cancel while a segment is in flight → the coordinator drains it
 * → gso_busy clears only when the group is fully retired (never mid-flight) → clean DETACHED close. */
UTEST(dgram_batch, gso_completion_close_with_inflight_segment) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &a));
    if (!(kl_event_caps(&ctx.loop) & KL_EVENT_CAP_COMPLETION)) { kl_event_ctx_free(&ctx); return; }  /* completion only */
    const KlSocketProvider *sp = ctx.sockets;
    int rxfd, port = mk_rx(&rxfd);
    ASSERT_NE(0, port);
    KlSocketHandle txfd = prep_fd(sp);
    KlDatagram tx; memset(&tx, 0, sizeof(tx));
    KlDatagramConfig cfg = { .ctx = &ctx, .alloc = &a, .sockets = sp, .fd = txfd,
                             .send_slots = 8, .send_slot_cap = 2048, .recv_cap = 2048 };
    ASSERT_EQ(0, kl_datagram_init_ex(&tx, &cfg, 0));
    KlDatagramBatch *b = kl_datagram_batch_create(&tx, KL_DGRAM_BATCH_SEND, 8, 2048);
    ASSERT_TRUE(b != NULL);

    KlSockAddr dest; kl_sockaddr_parse(&dest, "127.0.0.1", (uint16_t)port);
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send_gso(&tx, b, "AABBCC", 6, 2, &dest));
    ASSERT_EQ(1, b->gso_busy);   /* a fallback segment is now in flight / queued on the completion loop */

    /* Abortive close while the group is in flight: discard queued segments + cancel the in-flight one;
     * gso_busy clears as the group retires (the terminal completion / discard), never mid-flight. */
    ASSERT_EQ(0, kl_datagram_close_cancel(&tx));
    pump_close(&ctx, &tx);
    ASSERT_EQ((int)KL_DGRAM_CLOSE_CLOSED, (int)kl_datagram_close_state(&tx));
    ASSERT_EQ(0, b->gso_busy);   /* cleared exactly once, at safe retirement */

    ASSERT_EQ(0, kl_datagram_batch_free(b));   /* caller-owned; safe now that gso_busy is clear */
    close(rxfd);
    ASSERT_EQ(0, kl_datagram_free(&tx));
    kl_event_ctx_free(&ctx);
}

UTEST_MAIN();
