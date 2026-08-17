/*
 * test_stream_single_shot.c — R3b-T1: the single-shot completion contract for KlStream, exercised
 * through the REAL completion seam (no scripted mock).
 *
 * Contract (architecture_invariants.md I5): every completion backend emits EXACTLY ONE completion
 * per submitted op — no duplicate, no post-retirement re-fire. The stream raw-`containerof` recovery
 * (KL_COMP_READ/WRITE → KlStream) leans on this: a retired op must never produce a second completion
 * that a reused slot could misinterpret.
 *
 * This drives a real KlStream over the build's completion backend (pollcomp = deterministic oracle;
 * native io_uring / IOCP validation via CI enrollment). For each of READ and WRITE it posts ONE op,
 * drains until the single terminal completion arrives, then — WITHOUT rearming/resubmitting —
 * triggers additional peer activity (keeping the fd readable/writable) and drains again, asserting NO
 * second completion. On a readiness build (no submitted-op model) the property is N/A and the test
 * skips.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/event_ctx.h>
#include <keel/event.h>
#include <keel/stream.h>
#include <keel/stream_detail.h>     /* struct KlStream layout — set fd/ctx/alloc for a bare stream */
#include <keel/socket.h>            /* KlIoVec */
#include "../src/completion.h"      /* kl_comp_post_recv_raw/_send_raw, KlCompletionEvent, KL_COMP_* */
#include "../src/event_caps.h"      /* kl_event_caps */
#include "net_compat.h"
#include <string.h>

/* Count completions the real backend delivers for our stream (mirrors comp_server_conn_dispatch). */
static int       g_reads, g_writes;
static KlStream *g_target;
static void count_dispatch(struct KlEventCtx *ctx, const void *evp) {
    (void)ctx;
    const KlCompletionEvent *ev = evp;
    if (ev->target != g_target) return;
    if (ev->kind == KL_COMP_READ)       g_reads++;
    else if (ev->kind == KL_COMP_WRITE) g_writes++;
}

UTEST(stream_single_shot, one_completion_per_op) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(kl_event_ctx_init(&ctx, &a), 0);

    if (!(kl_event_caps(&ctx.loop) & KL_EVENT_CAP_COMPLETION)) {
        /* Readiness build: no submitted-op / completion model — single-shot is N/A here. Skip. */
        kl_event_ctx_free(&ctx);
        return;
    }

    ctx.comp_conn_dispatch = count_dispatch;   /* route KL_COMP_READ/WRITE to our counter */
    g_reads = 0; g_writes = 0;

    int sp[2];
    ASSERT_EQ(kl_test_socketpair(sp), 0);
    kl_test_set_nonblock(sp[0]); kl_test_set_nonblock(sp[1]);

    KlStream st; memset(&st, 0, sizeof(st));
    st.fd = (KlSocketHandle)sp[0]; st.ctx = &ctx; st.alloc = &a;
    g_target = &st;

    /* Associate the fd with the loop exactly as the completion server does at accept
     * (completion_server.c) — inert-ish on pollcomp/io_uring, CreateIoCompletionPort on IOCP. */
    ASSERT_EQ(kl_event_add(&ctx.loop, st.fd, KL_EVENT_READ, &st), 0);

    /* ── READ: one posted recv → exactly one completion ────────────────────────────────────────── */
    char rbuf[64];
    ASSERT_EQ(kl_comp_post_recv_raw(&st, rbuf, sizeof(rbuf)), 0);
    (void)kl_test_sockwrite(sp[1], "hello", 5);                 /* peer sends → sp[0] readable */
    for (int i = 0; i < 40 && g_reads == 0; i++) kl_event_ctx_run(&ctx, 16, 25);
    ASSERT_EQ(g_reads, 1);                                      /* the sole terminal completion */

    /* Do NOT repost. Trigger more peer activity (fd stays readable) and drain again. */
    (void)kl_test_sockwrite(sp[1], "world", 5);
    for (int i = 0; i < 8; i++) kl_event_ctx_run(&ctx, 16, 15);
    ASSERT_EQ(g_reads, 1);                                      /* single-shot: no second completion */

    /* ── WRITE: one posted send → exactly one completion ───────────────────────────────────────── */
    char wbuf[] = "data";
    KlIoVec iov = { .base = wbuf, .len = 4 };
    ASSERT_EQ(kl_comp_post_send_raw(&st, &iov, 1, 4), 0);
    for (int i = 0; i < 40 && g_writes == 0; i++) kl_event_ctx_run(&ctx, 16, 25);
    ASSERT_EQ(g_writes, 1);                                     /* the sole terminal completion */

    /* Do NOT resubmit. The socketpair stays writable; drain again. */
    (void)kl_test_sockread(sp[1], rbuf, sizeof(rbuf));          /* drain peer so sp[0] is clearly writable */
    for (int i = 0; i < 8; i++) kl_event_ctx_run(&ctx, 16, 15);
    ASSERT_EQ(g_writes, 1);                                     /* single-shot: no second completion */

    kl_event_del(&ctx.loop, st.fd);
    kl_test_closesock(sp[0]); kl_test_closesock(sp[1]);
    kl_event_ctx_free(&ctx);
}

UTEST_MAIN();
