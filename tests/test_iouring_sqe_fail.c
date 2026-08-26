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
#include "net_compat.h"
#include <string.h>

/* Defined in the -DKEEL_IOURING_TEST_HOOKS copy of event_iouring.c linked with this test. */
void kl_iou_test_fail_next_sqe(struct KlEventCtx *ctx, int count);

static int       g_writes;
static KlStream *g_target;
static void count_dispatch(struct KlEventCtx *ctx, const void *evp) {
    (void)ctx;
    const KlCompletionEvent *ev = evp;
    if (ev->target == g_target && ev->kind == KL_COMP_WRITE) g_writes++;
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

UTEST_MAIN()
