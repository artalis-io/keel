/*
 * test_datagram_batch.c — datagram M5.1 scaffolding: the batch object lifecycle (create/free), the
 * capability bits + accepted_rx_caps masking, the completion-creation rules (E2), overflow-safe
 * allocation + allocation-failure unwind, and the KL_IO_UNSUPPORTED mapping. Whitebox (reads the
 * internal batch + core layout). No send/recv wiring is exercised (that lands in M5.2/M5.3).
 */
#include "../vendor/utest.h"

#include <keel/datagram.h>
#include <keel/datagram_detail.h>
#include <keel/datagram_batch.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/udp.h>            /* KlUdpConfig — the provider configure() arg */

#include "../src/socket.h"       /* kl_sock_* seam, kl_sockdef_io_status */
#include "../src/datagram_core.h"/* KlDgramCore — whitebox accepted_rx_caps */
#include "../src/datagram_batch.h"/* struct KlDatagramBatch — whitebox blocks/owner/dir */

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
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

UTEST_MAIN();
