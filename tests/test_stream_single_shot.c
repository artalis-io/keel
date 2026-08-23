/*
 * test_stream_single_shot.c — the single-shot completion contract for KlStream, exercised
 * through the REAL completion seam (no scripted mock).
 *
 * Contract (architecture_invariants.md I5): every completion backend emits EXACTLY ONE completion
 * per submitted op — no duplicate, no post-retirement re-fire. The stream raw-`containerof` recovery
 * (KL_COMP_READ/WRITE → KlStream) leans on this.
 *
 * Drives a real KlStream over the build's completion backend (pollcomp = deterministic oracle;
 * native io_uring / IOCP via CI enrollment). For each of READ and WRITE it posts ONE op, drains
 * until the single terminal completion arrives, then — WITHOUT rearming/resubmitting — triggers
 * additional peer activity (keeping the fd readable/writable) and drains again, asserting NO second
 * completion. On a readiness build (no submitted-op model) the property is N/A and the test skips.
 *
 * Handles are full-width KlSocketHandle throughout (the loopback pair is built via the KEEL default
 * socket seam, kl_sockdef_*), so the IOCP enrollment operates on real pointer-width, overlapped-
 * capable SOCKETs — not int-truncated test handles. All peer transfers and drain results are
 * asserted, so a failed peer write or drain cannot masquerade as "no duplicate".
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/event_ctx.h>
#include <keel/event.h>
#include <keel/stream.h>
#include <keel/stream_detail.h>     /* struct KlStream layout — set fd/ctx/alloc for a bare stream */
#include <keel/socket.h>            /* KlIoVec */
#include <keel/sockaddr.h>          /* KlSockAddr, kl_sockaddr_from_ipv4 */
#include <keel/handle.h>            /* KlSocketHandle, kl_handle_valid */
#include "../src/socket.h"          /* kl_sockdef_* default socket seam (KlSocketHandle-native) */
#include "../src/completion.h"      /* kl_comp_post_recv_raw/_send_raw, KlCompletionEvent, KL_COMP_* */
#include "../src/event_caps.h"      /* kl_event_caps */
#include "net_compat.h"             /* portable platform-socket include boundary → AF_INET/SOCK_STREAM */
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

/* A connected TCP loopback pair via the default socket seam — full-width KlSocketHandle on every
 * platform (overlapped-capable on IOCP), unlike int-narrowed test socketpairs. */
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
    if (kl_sockdef_connect(cli, &bound) < 0) goto fail;      /* loopback blocking connect completes */
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

    KlSocketHandle end, peer;
    ASSERT_EQ(make_pair(&end, &peer), 0);
    ASSERT_EQ(kl_sockdef_set_nonblocking(end), 0);
    ASSERT_EQ(kl_sockdef_set_nonblocking(peer), 0);

    KlStream st; memset(&st, 0, sizeof(st));
    st.fd = end; st.ctx = &ctx; st.alloc = &a;
    g_target = &st;

    /* Associate the fd with the loop exactly as the completion server does at accept
     * (completion_http_server.c) — inert-ish on pollcomp/io_uring, CreateIoCompletionPort on IOCP. */
    ASSERT_EQ(kl_event_add(&ctx.loop, st.fd, KL_EVENT_READ, &st), 0);

    /* ── READ: one posted recv → exactly one completion ────────────────────────────────────────── */
    char rbuf[64];
    ASSERT_EQ(kl_comp_post_recv_raw(&st, rbuf, sizeof(rbuf)), 0);
    ASSERT_EQ((long)kl_sockdef_send(peer, "hello", 5), 5L);        /* peer sends → `end` readable */
    for (int i = 0; i < 40 && g_reads == 0; i++)
        ASSERT_TRUE(kl_event_ctx_run(&ctx, 16, 25) >= 0);
    ASSERT_EQ(g_reads, 1);                                         /* the sole terminal completion */

    /* Do NOT repost. Trigger REAL additional peer activity (asserted) and drain again. */
    ASSERT_EQ((long)kl_sockdef_send(peer, "world", 5), 5L);        /* `end` genuinely readable again */
    for (int i = 0; i < 8; i++)
        ASSERT_TRUE(kl_event_ctx_run(&ctx, 16, 15) >= 0);
    ASSERT_EQ(g_reads, 1);                                         /* single-shot: no second completion */

    /* ── WRITE: one posted send → exactly one completion ───────────────────────────────────────── */
    char wbuf[] = "data";
    KlIoVec iov = { .base = wbuf, .len = 4 };
    ASSERT_EQ(kl_comp_post_send_raw(&st, &iov, 1, 4), 0);
    for (int i = 0; i < 40 && g_writes == 0; i++)
        ASSERT_TRUE(kl_event_ctx_run(&ctx, 16, 25) >= 0);
    ASSERT_EQ(g_writes, 1);                                        /* the sole terminal completion */

    /* The send actually transferred (real completion, not spurious): the peer receives exactly it. */
    char pbuf[64]; long got = 0;
    for (int i = 0; i < 40 && got < 4; i++) {
        long r = (long)kl_sockdef_recv(peer, pbuf + got, sizeof(pbuf) - (size_t)got);
        if (r > 0) got += r;
        else ASSERT_TRUE(kl_event_ctx_run(&ctx, 16, 15) >= 0);    /* let the write drain if pending */
    }
    ASSERT_EQ(got, 4L);
    ASSERT_EQ(memcmp(pbuf, "data", 4), 0);

    /* Do NOT resubmit. `end` stays writable; drain again → no second write completion. */
    for (int i = 0; i < 8; i++)
        ASSERT_TRUE(kl_event_ctx_run(&ctx, 16, 15) >= 0);
    ASSERT_EQ(g_writes, 1);                                        /* single-shot: no second completion */

    kl_event_del(&ctx.loop, st.fd);
    kl_sockdef_close(end); kl_sockdef_close(peer);
    kl_event_ctx_free(&ctx);
}

UTEST_MAIN();
