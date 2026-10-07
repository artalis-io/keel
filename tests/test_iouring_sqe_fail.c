/*
 * test_iouring_sqe_fail.c: deterministic regression for the io_uring L2 fix - when the INITIAL
 * send/sendfile submission cannot obtain an SQE (submission-queue exhaustion), the post must FAIL
 * (unlink + release the op's buffer + free it) rather than strand a WRITE that never completes.
 *
 * SQ exhaustion is not reproducible on demand through the real ring, so this test links a copy of
 * event_iouring.c built with -DKEEL_IOURING_TEST_HOOKS (a compile-time-guarded seam absent from
 * production) that forces the next iou_sqe() to return NULL. It drives a REAL io_uring loop + a
 * real loopback stream through the neutral completion seam (kl_comp_post_send_raw), so the actual
 * iou_comp_post_send path runs. io_uring-only: enrolled solely in IOURING_TEST_SUITES.
 *
 * Asserts, for the forced-!sqe initial post: (1) the post returns failure; (2) no WRITE completion
 * is ever awaited/delivered for it; (3) the loop keeps working - a subsequent send succeeds and the
 * peer receives it, proving the op was unlinked and its registered buffer released for reuse;
 * (4) clean shutdown. The "unlinked + freed + released exactly once" ownership claim is additionally
 * enforced by ASan/UBSan/LSan on the io_uring CI run (a leaked op/buffer, a double free, or a stale
 * op left in st->ops would trip the sanitizers at teardown).
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/event_ctx.h>
#include <keel/event.h>
#include <keel/stream.h>
#include <keel/stream_detail.h>
#include <keel/socket.h>
#include <keel/sockaddr.h>
#include <keel/handle.h>
#include "../src/socket.h"
#include "../src/completion.h"
#include "../src/event_caps.h"
#include "../src/completion_io.h"   /* kl_comp_cancel: the idle-sweep cancel */
#include "net_compat.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>

/* Defined in the -DKEEL_IOURING_TEST_HOOKS copy of event_iouring.c linked with this test. */
void kl_iou_test_fail_next_sqe(struct KlEventCtx *ctx, int count);
void kl_iou_test_send_cap(struct KlEventCtx *ctx, size_t cap);
size_t kl_iou_test_send_prepared_max(struct KlEventCtx *ctx);
void kl_iou_test_send_res0_next(struct KlEventCtx *ctx, int count);
void kl_iou_test_submit_rc_next(struct KlEventCtx *ctx, int rc);

static int       g_writes, g_write_fails, g_reads, g_read_fails;
static KlStream *g_target;
static void count_dispatch(struct KlEventCtx *ctx, const void *evp) {
    (void)ctx;
    const KlCompletionEvent *ev = evp;
    if (ev->target != g_target) return;
    if (ev->kind == KL_COMP_WRITE) { g_writes++; if (!ev->ok) g_write_fails++; }
    if (ev->kind == KL_COMP_READ)  { g_reads++;  if (!ev->ok) g_read_fails++; }
}

static int make_pair(KlSocketHandle *end, KlSocketHandle *peer) {
    KlSocketHandle lis = kl_sockdef_socket(AF_INET, SOCK_STREAM, 0);
    if (!kl_handle_valid(lis)) return -1;
    uint8_t lo[4] = { 127, 0, 0, 1 };
    KlSockAddr addr; kl_sockaddr_from_ipv4(&addr, lo, 0);
    KlSockAddr bound;
    KlSocketHandle cli = KL_INVALID_SOCKET, srv = KL_INVALID_SOCKET;
    if (kl_sockdef_bind(lis, &addr) < 0 || kl_sockdef_listen(lis, 1) < 0) goto fail;
    if (kl_sockdef_get_local_addr(lis, &bound) < 0) goto fail;
    cli = kl_sockdef_socket(AF_INET, SOCK_STREAM, 0);
    if (!kl_handle_valid(cli)) goto fail;
    if (kl_sockdef_connect(cli, &bound) < 0) goto fail;
    srv = kl_sockdef_accept(lis, NULL);
    if (!kl_handle_valid(srv)) goto fail;
    kl_sockdef_close(lis);
    *end = cli; *peer = srv;
    return 0;
fail:
    if (kl_handle_valid(lis)) kl_sockdef_close(lis);
    if (kl_handle_valid(cli)) kl_sockdef_close(cli);
    return -1;
}

/* Small send -> registered (WRITE_FIXED) buffer path. Forced SQ-exhaustion at the initial post
 * must fail cleanly; the loop then keeps working (buffer released, op unlinked/freed). */
UTEST(iouring_sqe_fail, reg_buffer_initial_post_fails_and_recovers) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(kl_event_ctx_init(&ctx, &a), 0);
    if (!(kl_event_caps(&ctx.loop) & KL_EVENT_CAP_COMPLETION)) { kl_event_ctx_free(&ctx); return; }
    ctx.comp_conn_dispatch = count_dispatch;
    g_writes = 0;

    KlSocketHandle end, peer;
    ASSERT_EQ(make_pair(&end, &peer), 0);
    ASSERT_EQ(kl_sockdef_set_nonblocking(end), 0);
    ASSERT_EQ(kl_sockdef_set_nonblocking(peer), 0);

    KlStream st; memset(&st, 0, sizeof(st));
    st.fd = end; st.ctx = &ctx; st.alloc = &a;
    g_target = &st;
    ASSERT_EQ(kl_event_add(&ctx.loop, st.fd, KL_EVENT_READ, &st), 0);

    char wbuf[] = "data";
    KlIoVec iov = { .base = wbuf, .len = 4 };

    /* (1) Forced SQ exhaustion at the initial post -> the post FAILS. */
    kl_iou_test_fail_next_sqe(&ctx, 1);
    ASSERT_EQ(kl_comp_post_send_raw(&st, &iov, 1, 4), -1);

    /* (2) No WRITE completion is awaited/delivered for the failed op. */
    for (int i = 0; i < 8; i++) ASSERT_TRUE(kl_event_ctx_run(&ctx, 16, 15) >= 0);
    ASSERT_EQ(g_writes, 0);

    /* (3) The loop still works: a subsequent send succeeds and the peer receives it, proving the
     * op was unlinked and the registered buffer released for reuse. */
    ASSERT_EQ(kl_comp_post_send_raw(&st, &iov, 1, 4), 0);
    for (int i = 0; i < 40 && g_writes == 0; i++) ASSERT_TRUE(kl_event_ctx_run(&ctx, 16, 25) >= 0);
    ASSERT_EQ(g_writes, 1);
    char pbuf[64]; long got = 0;
    for (int i = 0; i < 40 && got < 4; i++) {
        long r = (long)kl_sockdef_recv(peer, pbuf + got, sizeof(pbuf) - (size_t)got);
        if (r > 0) got += r;
        else ASSERT_TRUE(kl_event_ctx_run(&ctx, 16, 15) >= 0);
    }
    ASSERT_EQ(got, 4L);
    ASSERT_EQ(memcmp(pbuf, "data", 4), 0);

    /* (4) Clean shutdown. */
    kl_event_del(&ctx.loop, st.fd);
    kl_sockdef_close(end); kl_sockdef_close(peer);
    kl_event_ctx_free(&ctx);
}

/* Large send -> malloc'd buffer path. Forced SQ-exhaustion at the initial post must fail cleanly
 * and free the malloc'd send buffer exactly once (LSan/ASan enforce this at teardown). */
UTEST(iouring_sqe_fail, malloc_buffer_initial_post_fails_cleanly) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(kl_event_ctx_init(&ctx, &a), 0);
    if (!(kl_event_caps(&ctx.loop) & KL_EVENT_CAP_COMPLETION)) { kl_event_ctx_free(&ctx); return; }
    ctx.comp_conn_dispatch = count_dispatch;
    g_writes = 0;

    KlSocketHandle end, peer;
    ASSERT_EQ(make_pair(&end, &peer), 0);
    ASSERT_EQ(kl_sockdef_set_nonblocking(end), 0);
    ASSERT_EQ(kl_sockdef_set_nonblocking(peer), 0);

    KlStream st; memset(&st, 0, sizeof(st));
    st.fd = end; st.ctx = &ctx; st.alloc = &a;
    g_target = &st;
    ASSERT_EQ(kl_event_add(&ctx.loop, st.fd, KL_EVENT_READ, &st), 0);

    /* A payload larger than any registered buffer forces the malloc + SEND path. */
    static char big[65536];
    memset(big, 'z', sizeof(big));
    KlIoVec iov = { .base = big, .len = sizeof(big) };

    kl_iou_test_fail_next_sqe(&ctx, 1);
    ASSERT_EQ(kl_comp_post_send_raw(&st, &iov, 1, sizeof(big)), -1);   /* post fails, sendbuf freed once */
    for (int i = 0; i < 8; i++) ASSERT_TRUE(kl_event_ctx_run(&ctx, 16, 15) >= 0);
    ASSERT_EQ(g_writes, 0);                                            /* no completion awaited */

    kl_event_del(&ctx.loop, st.fd);
    kl_sockdef_close(end); kl_sockdef_close(peer);
    kl_event_ctx_free(&ctx);
}

/* A cancel that finds no SQE is retried at the next drain, not lost (audit L4). The idle sweep's
 * kl_comp_cancel marks the op aborted and posts an ASYNC_CANCEL; when no SQE was free the cancel was
 * simply skipped, so a recv on a quiet socket never completed and its connection never retired. */
UTEST(iouring_sqe_fail, cancel_without_an_sqe_is_retried) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(kl_event_ctx_init(&ctx, &a), 0);
    if (!(kl_event_caps(&ctx.loop) & KL_EVENT_CAP_COMPLETION)) { kl_event_ctx_free(&ctx); return; }
    ctx.comp_conn_dispatch = count_dispatch;
    g_reads = g_read_fails = 0;

    KlSocketHandle end, peer;
    ASSERT_EQ(make_pair(&end, &peer), 0);
    ASSERT_EQ(kl_sockdef_set_nonblocking(end), 0);
    ASSERT_EQ(kl_sockdef_set_nonblocking(peer), 0);
    KlStream st; memset(&st, 0, sizeof(st));
    st.fd = end; st.ctx = &ctx; st.alloc = &a;
    g_target = &st;
    ASSERT_EQ(kl_event_add(&ctx.loop, st.fd, KL_EVENT_READ, &st), 0);

    static char rbuf[256];
    ASSERT_EQ(kl_comp_post_recv_raw(&st, rbuf, sizeof(rbuf)), 0);   /* the peer never writes */
    for (int i = 0; i < 4; i++) ASSERT_TRUE(kl_event_ctx_run(&ctx, 16, 5) >= 0);   /* submitted */
    ASSERT_EQ(g_reads, 0);

    kl_iou_test_fail_next_sqe(&ctx, 1);                  /* the cancel finds no SQE */
    kl_comp_cancel(&ctx, st.fd);
    for (int i = 0; i < 40 && g_reads == 0; i++) ASSERT_TRUE(kl_event_ctx_run(&ctx, 16, 25) >= 0);
    ASSERT_EQ(g_reads, 1);                               /* was: 0, the recv never completed */
    ASSERT_EQ(g_read_fails, 1);                          /* delivered as the cancelled error */

    kl_event_del(&ctx.loop, st.fd);
    kl_sockdef_close(end); kl_sockdef_close(peer);
    kl_event_ctx_free(&ctx);
}

/* A short send whose tail re-prep finds no SQE fails the write now instead of stranding it (L4). The
 * peer does not read and its receive buffer is small, so a large send completes short; the forced
 * SQE failure then hits the tail's re-prep. Before the fix the op was marked aborted with nothing
 * queued, so no WRITE completion ever arrived and the connection hung. */
UTEST(iouring_sqe_fail, short_send_tail_without_an_sqe_fails_the_write) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(kl_event_ctx_init(&ctx, &a), 0);
    if (!(kl_event_caps(&ctx.loop) & KL_EVENT_CAP_COMPLETION)) { kl_event_ctx_free(&ctx); return; }
    ctx.comp_conn_dispatch = count_dispatch;
    g_writes = g_write_fails = 0;

    KlSocketHandle end, peer;
    ASSERT_EQ(make_pair(&end, &peer), 0);
    ASSERT_EQ(kl_sockdef_set_nonblocking(end), 0);
    ASSERT_EQ(kl_sockdef_set_nonblocking(peer), 0);
    int small = 4096;
    (void)setsockopt((int)peer, SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
    (void)setsockopt((int)end, SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
    KlStream st; memset(&st, 0, sizeof(st));
    st.fd = end; st.ctx = &ctx; st.alloc = &a;
    g_target = &st;
    ASSERT_EQ(kl_event_add(&ctx.loop, st.fd, KL_EVENT_READ, &st), 0);

    static char big[4 * 1024 * 1024];
    memset(big, 'y', sizeof(big));
    KlIoVec iov = { .base = big, .len = sizeof(big) };
    ASSERT_EQ(kl_comp_post_send_raw(&st, &iov, 1, sizeof(big)), 0);   /* consumes its own SQE */
    kl_iou_test_fail_next_sqe(&ctx, 1);                   /* the tail's re-prep finds none */
    for (int i = 0; i < 60 && g_writes == 0; i++) ASSERT_TRUE(kl_event_ctx_run(&ctx, 16, 25) >= 0);
    ASSERT_EQ(g_writes, 1);                               /* was: 0, stranded */
    ASSERT_EQ(g_write_fails, 1);

    kl_event_del(&ctx.loop, st.fd);
    kl_sockdef_close(end); kl_sockdef_close(peer);
    kl_event_ctx_free(&ctx);
}

/* A watcher removed while no SQE is free: kl_event_del dropped the POLL_REMOVE, so the poll stayed
 * in the kernel. It holds a reference to the socket's file, so closing the descriptor did not close
 * the connection (the peer never saw EOF), and the watch stayed allocated until the loop closed. The
 * remove is now retried at the next drain, like an unsent cancel. */
static long g_live;
static void *lc_malloc(void *c, size_t n) { (void)c; void *p = malloc(n ? n : 1); if (p) g_live++; return p; }
static void *lc_realloc(void *c, void *p, size_t o, size_t n) { (void)c; (void)o; void *q = realloc(p, n ? n : 1); if (q && !p) g_live++; return q; }
static void lc_free(void *c, void *p, size_t n) { (void)c; (void)n; if (p) { g_live--; free(p); } }
static void never_ready(KlSocketHandle fd, KlEventMask ready, void *ud) { (void)fd; (void)ready; (void)ud; }

UTEST(iouring_sqe_fail, watcher_remove_without_an_sqe_is_retried) {
    KlAllocator a = { lc_malloc, lc_realloc, lc_free, NULL };
    KlEventCtx ctx;
    ASSERT_EQ(kl_event_ctx_init(&ctx, &a), 0);
    if (!(kl_event_caps(&ctx.loop) & KL_EVENT_CAP_COMPLETION)) { kl_event_ctx_free(&ctx); return; }

    KlSocketHandle end, peer;
    ASSERT_EQ(make_pair(&end, &peer), 0);
    ASSERT_EQ(kl_sockdef_set_nonblocking(peer), 0);
    long base = g_live;
    ASSERT_EQ(kl_watcher_add(&ctx, end, KL_EVENT_READ, never_ready, NULL), 0);   /* the peer never writes */
    for (int i = 0; i < 4; i++) ASSERT_TRUE(kl_event_ctx_run(&ctx, 16, 5) >= 0);   /* the poll is armed */

    kl_iou_test_fail_next_sqe(&ctx, 1);                  /* the POLL_REMOVE finds no SQE */
    kl_watcher_del(&ctx, end);
    kl_sockdef_close(end);
    for (int i = 0; i < 10; i++) ASSERT_TRUE(kl_event_ctx_run(&ctx, 16, 10) >= 0);

    char b[8];
    long n = (long)recv((int)peer, b, sizeof b, 0);
    long live = g_live;
    kl_sockdef_close(peer);
    kl_event_ctx_free(&ctx);
    ASSERT_EQ(n, 0L);                                    /* EOF; was: -1 (the file stayed open) */
    ASSERT_EQ(live, base);                               /* the watch was freed */
}

/* Shared setup for the send-length and submit-return tests: a completion loop, a loopback pair and
 * a stream over one end. Returns 0, or 1 when this loop is not a completion loop (skip). */
typedef struct {
    KlAllocator    a;
    KlEventCtx     ctx;
    KlSocketHandle end, peer;
    KlStream       st;
} IouRig;

static int rig_open(IouRig *r) {
    r->a = kl_allocator_default();
    if (kl_event_ctx_init(&r->ctx, &r->a) != 0) return -1;
    if (!(kl_event_caps(&r->ctx.loop) & KL_EVENT_CAP_COMPLETION)) { kl_event_ctx_free(&r->ctx); return 1; }
    r->ctx.comp_conn_dispatch = count_dispatch;
    g_writes = g_write_fails = g_reads = g_read_fails = 0;
    if (make_pair(&r->end, &r->peer) != 0) return -1;
    if (kl_sockdef_set_nonblocking(r->end) != 0 || kl_sockdef_set_nonblocking(r->peer) != 0) return -1;
    memset(&r->st, 0, sizeof(r->st));
    r->st.fd = r->end; r->st.ctx = &r->ctx; r->st.alloc = &r->a;
    g_target = &r->st;
    return kl_event_add(&r->ctx.loop, r->st.fd, KL_EVENT_READ, &r->st) == 0 ? 0 : -1;
}

static void rig_close(IouRig *r) {
    kl_event_del(&r->ctx.loop, r->st.fd);
    kl_sockdef_close(r->end); kl_sockdef_close(r->peer);
    kl_event_ctx_free(&r->ctx);
}

/* Drive the loop until the one WRITE completes, reading the peer as it goes. Returns bytes read. */
static long rig_pump(IouRig *r, char *dst, long want) {
    long got = 0;
    for (int i = 0; i < 2000 && (g_writes == 0 || got < want); i++) {
        long n = (long)kl_sockdef_recv(r->peer, dst + got, (size_t)(want - got));
        if (n > 0) { got += n; continue; }
        if (kl_event_ctx_run(&r->ctx, 16, 5) < 0) break;
        if (g_writes && g_write_fails) break;
    }
    return got;
}

/* sqe->len is 32 bits: a send remainder of exactly 4 GiB was prepared as length 0, the kernel
 * returned 0, and the send was re-prepared forever. Each prepared length is now capped and the
 * rest goes out as further sends. The cap is lowered here so a 64 KiB send takes the split a 4 GiB
 * one takes in production; the whole payload must arrive, in order, as one WRITE completion. */
UTEST(iouring_sqe_fail, send_length_is_capped_per_sqe) {
    IouRig r;
    int rc = rig_open(&r);
    if (rc == 1) return;
    ASSERT_EQ(rc, 0);

    /* The backend advertises the cap, so a caller that splits its output posts no more than it. */
    size_t smax = kl_comp_send_max_raw(&r.st);
    EXPECT_TRUE(smax > 0);                                    /* was 0: no limit */
    EXPECT_TRUE(smax <= (size_t)0x7ffff000u);

    static char big[65536], got_buf[65536];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (char)(i * 7u + 3u);
    KlIoVec iov = { .base = big, .len = sizeof(big) };
    kl_iou_test_send_cap(&r.ctx, 1000);
    ASSERT_EQ(kl_comp_post_send_raw(&r.st, &iov, 1, sizeof(big)), 0);
    long got = rig_pump(&r, got_buf, (long)sizeof(big));
    size_t prepared_max = kl_iou_test_send_prepared_max(&r.ctx);
    rig_close(&r);

    EXPECT_TRUE(prepared_max <= 1000u);                      /* was 65536: one uncapped SQE */
    ASSERT_EQ(g_writes, 1);
    ASSERT_EQ(g_write_fails, 0);
    ASSERT_EQ(got, (long)sizeof(big));
    ASSERT_EQ(memcmp(got_buf, big, sizeof(big)), 0);
}

/* A send of a non-empty remainder that completes with 0 bytes cannot make progress (it is what a
 * 0-length SQE returns), so it fails the write instead of being re-prepared. */
UTEST(iouring_sqe_fail, zero_result_on_a_nonempty_send_fails_the_write) {
    IouRig r;
    int rc = rig_open(&r);
    if (rc == 1) return;
    ASSERT_EQ(rc, 0);

    static char big[65536], sink[65536];
    memset(big, 'q', sizeof(big));
    KlIoVec iov = { .base = big, .len = sizeof(big) };
    kl_iou_test_send_res0_next(&r.ctx, 1);
    ASSERT_EQ(kl_comp_post_send_raw(&r.st, &iov, 1, sizeof(big)), 0);
    (void)rig_pump(&r, sink, (long)sizeof(sink));
    rig_close(&r);

    ASSERT_EQ(g_writes, 1);
    ASSERT_EQ(g_write_fails, 1);                             /* was 0: re-prepared and resent */
}

/* io_uring_enter returns -EBUSY (or -EAGAIN) while a CQ overflow backlog is pending (kernels 5.5 to
 * 5.18 with NODROP). That is a request to reap, not a loop failure: the drain must go on to reap
 * the completions, and the loop keeps working. */
static void submit_busy_case(int *ok_out, int errcode) {
    IouRig r;
    *ok_out = 0;
    int rc = rig_open(&r);
    if (rc == 1) { *ok_out = 1; return; }
    if (rc != 0) return;

    char wbuf[] = "ping", pbuf[16];
    KlIoVec iov = { .base = wbuf, .len = 4 };
    if (kl_comp_post_send_raw(&r.st, &iov, 1, 4) != 0) { rig_close(&r); return; }
    kl_iou_test_submit_rc_next(&r.ctx, -errcode);
    int run = kl_event_ctx_run(&r.ctx, 16, 25);              /* was -1: fatal to the loop */
    long got = rig_pump(&r, pbuf, 4);
    rig_close(&r);
    *ok_out = run >= 0 && g_writes == 1 && g_write_fails == 0 && got == 4 &&
              memcmp(pbuf, "ping", 4) == 0;
}

UTEST(iouring_sqe_fail, submit_ebusy_reaps_instead_of_failing) {
    int ok = 0;
    submit_busy_case(&ok, EBUSY);
    ASSERT_TRUE(ok);
}

UTEST(iouring_sqe_fail, submit_eagain_reaps_instead_of_failing) {
    int ok = 0;
    submit_busy_case(&ok, EAGAIN);
    ASSERT_TRUE(ok);
}

UTEST_MAIN()
