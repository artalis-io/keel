/*
 * raw_udp_test.c: public KlDatagram round-trip over the lwIP raw completion backend.
 *
 * The UDP analog of the TCP client loopback test. It brings up ONE raw ctx
 * (kl_event_ctx_init_ex + kl_event_provider_lwip_raw → NO_SYS=1 lwIP + loopback netif) and runs
 * KEEL's public KlDatagram machine (src/datagram.c) over the raw datagram data-plane:
 *
 *   - kl_datagram_socket_init creates a udp_pcb for SOCK_DGRAM (via lwr_sock_socket),
 *   - kl_datagram_recv_start posts an overlapped recv (kl_comp_post_dgram_recv → udp_recv arm),
 *   - kl_datagram_send posts a send (kl_comp_post_dgram_send → udp_sendto),
 *   - the mainloop tick (kl_event_ctx_run → lwr_comp_drain) delivers the datagram back as a
 *     KL_COMP_DGRAM_RECV, which the driver turns into the on_recv callback.
 *
 * Coverage:
 *   T1  a single datagram round-trips byte-exact, with the correct source address (127.0.0.1).
 *   T2  a few datagrams back-to-back all arrive in order.
 *   T3  a datagram at the per-udp DGRAM_MAX bound round-trips (largest retained payload).
 *   T4  close-with-armed-recv: a receive is armed when the socket is closed; exercises the recv-side
 *         token release-on-close (arm ref, via the KlDatagram close coordinator's recv cancel/terminal)
 *         + the drain transfer path for the sends (T4 pumps, so its sends are transferred, not released).
 *   T5  close-with-undrained-sends: the SENDER is closed with pending sends still queued in the backend
 *         (no drain ran), so its pending sends take the close-side cancel/release path (T4 cannot cover
 *         this: the drain consumes all pending sends of a slot in one tick). Isolates the send-side
 *         retained-ref release under the public close.
 *   T6  one-held-packet overflow, driven at the GLUE level (kl_lwr_udp_*, own ctx): two datagrams
 *         reach the udp_recv callback in one netif_poll (synchronous glue sends, before any drain): the
 *         backend holds exactly ONE and DROPS the second (no 16-entry ring), so precisely one is
 *         delivered. The public KlDatagram send posts a completion op flushed during a tick, so it cannot
 *         force both into one poll (an eager synchronous udp_sendto would): the property lives in the
 *         backend, so it is asserted at the glue seam like T7.
 *   T7  no stale capture while UNARMED, driven at the GLUE level (kl_lwr_udp_*, its own ctx after
 *         the event ctx is freed): a datagram arriving with no recv armed is DROPPED at the callback, so
 *         a later post_recv surfaces NOTHING stale. Not observable through the public KlDatagram API
 *         (recv_stop is terminal in KlDgramRecv + the dispatch drops on recv_active==0), so this proves
 *         the gate one layer down.
 *
 * Diagnostic: the whole suite runs under ASan+UBSan+LSan, so every case doubles as a leak/
 * double-free check on the stable-token retain/transfer/release discipline (no independent "no-leak"
 * case: LSan validates the suite as a whole).
 *
 * A real datagram crosses the loopback netif through KEEL's datagram.c machine: src/datagram.c and the
 * lwIP-raw provider + glue are UNCHANGED; only the public transport driver changes. Prints its PASS marker on success.
 *
 * SPDX-License-Identifier: MIT
 */
#include <keel/keel.h>
#include <keel/datagram.h>
#include <keel/datagram_detail.h>   /* complete KlDatagram type: stack instances below */
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/sockaddr.h>

#include "keel_lwip_raw.h"
#include "lwip_raw_glue.h"   /* T7 drives the datagram glue directly (kl_lwr_udp_* / kl_lwr_ctx_*) */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* The stable-liveness token (src/datagram_life.h), forward-declared so this test manages op tokens for
 * the glue-level T7 without pulling the src/ header. Resolves against libkeel's datagram_life.o.
 * (create takes a `dispatch` completion-routing handler: unused at the glue level here, so NULL.) */
typedef struct KlDgramLife KlDgramLife;
struct KlCompletionEvent;
typedef void (*KlDgramDispatchFn)(void *target, const struct KlCompletionEvent *ev);
KlDgramLife *kl_dgram_life_create(KlAllocator *alloc, void *target, void (*on_final)(void *), void *final_ctx,
                                  KlDgramDispatchFn dispatch);
void         kl_dgram_life_release(KlDgramLife *l);
static void  t7_noop_final(void *ctx) { (void)ctx; }

static int fail(const char *msg) { printf("LC-3a FAIL: %s\n", msg); return 1; }

/* ── captured receive state ─────────────────────────────────────────────── */
static char   g_buf[4096];
static size_t g_len;
static int    g_got;
static char   g_src_ip[KL_SOCKADDR_STRLEN];

static void on_recv(void *ud, const void *data, size_t len,
                    const KlSockAddr *peer, const KlSockAddr *local, unsigned flags) {
    (void)local; (void)ud; (void)flags;
    if (len > sizeof(g_buf)) len = sizeof(g_buf);
    memcpy(g_buf, data, len);
    g_len = len;
    g_src_ip[0] = '\0';
    if (peer) kl_sockaddr_format_ip(peer, g_src_ip, sizeof(g_src_ip));
    g_got++;
}

static void reset_capture(void) {
    g_len = 0; g_got = 0; g_src_ip[0] = '\0'; memset(g_buf, 0, sizeof(g_buf));
}

/* Public close lifecycle: abortive close_cancel -> pump the raw loop until CLOSED -> free.
 * Exercises the canonical KlDatagram teardown over the lwIP-raw provider (the close
 * coordinator's recv-cancel terminal + pending-send release). */
static int dg_close_free(KlEventCtx *ctx, KlDatagram *dg) {
    if (kl_datagram_close_state(dg) != KL_DGRAM_CLOSE_CLOSED)
        (void)kl_datagram_close_cancel(dg);
    for (int i = 0; i < 400 && kl_datagram_close_state(dg) != KL_DGRAM_CLOSE_CLOSED; i++)
        kl_event_ctx_run(ctx, 16, 5);
    if (kl_datagram_close_state(dg) != KL_DGRAM_CLOSE_CLOSED) { (void)fail("close did not reach CLOSED"); return -1; }
    return kl_datagram_free(dg);
}

/* Pump the raw mainloop up to `ticks` times or until g_got reaches `want`. */
static void pump_until(KlEventCtx *ctx, int want, int ticks) {
    for (int i = 0; i < ticks && g_got < want; i++)
        kl_event_ctx_run(ctx, 16, 5);
}

static void dest_v4(KlSockAddr *a, uint16_t port) {
    const uint8_t lo[4] = { 127, 0, 0, 1 };
    kl_sockaddr_from_ipv4(a, lo, port);
}

/* T1: one datagram, byte-exact + source addr. */
static int t1_single(KlEventCtx *ctx) {
    reset_capture();
    KlDatagram rx, tx;
    KlDatagramSocketConfig rc = { .ctx = ctx, .bind_addr = "127.0.0.1" };
    KlDatagramSocketConfig tc = { .ctx = ctx };
    if (kl_datagram_socket_init(&rx, &rc) != 0) return fail("rx init");
    if (kl_datagram_socket_init(&tx, &tc) != 0) { dg_close_free(ctx, &rx); return fail("tx init"); }
    if (kl_datagram_recv_start(&rx, on_recv, NULL) != 0) { dg_close_free(ctx, &tx); dg_close_free(ctx, &rx); return fail("recv_start"); }

    uint16_t port = kl_datagram_local_port(&rx);
    if (port == 0) { dg_close_free(ctx, &tx); dg_close_free(ctx, &rx); return fail("rx local port 0"); }

    KlSockAddr dst; dest_v4(&dst, port);
    const char *msg = "hello raw udp";
    if (kl_datagram_send(&tx, &(KlDatagramMessage){ .data = msg, .len = strlen(msg), .peer = &dst, .tos = -1 }) != KL_DATAGRAM_ACCEPTED) { dg_close_free(ctx, &tx); dg_close_free(ctx, &rx); return fail("send_to"); }

    pump_until(ctx, 1, 400);
    int rc2 = 0;
    if (g_got != 1) rc2 = fail("T1: datagram not received");
    else if (g_len != strlen(msg) || memcmp(g_buf, msg, g_len) != 0) rc2 = fail("T1: payload mismatch");
    else if (strcmp(g_src_ip, "127.0.0.1") != 0) rc2 = fail("T1: wrong source address");

    dg_close_free(ctx, &tx);
    dg_close_free(ctx, &rx);
    if (rc2) return rc2;
    printf("PASS T1 (single datagram round-trip, src=127.0.0.1)\n");
    return 0;
}

/* T2: a few datagrams back-to-back all arrive. */
static int t2_burst(KlEventCtx *ctx) {
    reset_capture();
    KlDatagram rx, tx;
    KlDatagramSocketConfig rc = { .ctx = ctx, .bind_addr = "127.0.0.1" };
    KlDatagramSocketConfig tc = { .ctx = ctx };
    if (kl_datagram_socket_init(&rx, &rc) != 0) return fail("rx init");
    if (kl_datagram_socket_init(&tx, &tc) != 0) { dg_close_free(ctx, &rx); return fail("tx init"); }
    if (kl_datagram_recv_start(&rx, on_recv, NULL) != 0) { dg_close_free(ctx, &tx); dg_close_free(ctx, &rx); return fail("recv_start"); }
    uint16_t port = kl_datagram_local_port(&rx);
    KlSockAddr dst; dest_v4(&dst, port);

    const int N = 5;
    char last[32];
    for (int i = 0; i < N; i++) {
        char m[32];
        int mn = snprintf(m, sizeof(m), "dgram-%d", i);
        if (i == N - 1) memcpy(last, m, (size_t)mn + 1);
        if (kl_datagram_send(&tx, &(KlDatagramMessage){ .data = m, .len = (size_t)mn, .peer = &dst, .tos = -1 }) != KL_DATAGRAM_ACCEPTED) { dg_close_free(ctx, &tx); dg_close_free(ctx, &rx); return fail("T2 send"); }
        /* pump between sends so each datagram is drained (one recv-in-flight per drain). */
        pump_until(ctx, i + 1, 100);
    }
    pump_until(ctx, N, 400);
    int rc2 = 0;
    if (g_got != N) rc2 = fail("T2: not all datagrams received");
    else if (g_len != strlen(last) || memcmp(g_buf, last, g_len) != 0) rc2 = fail("T2: last payload mismatch");

    dg_close_free(ctx, &tx);
    dg_close_free(ctx, &rx);
    if (rc2) return rc2;
    printf("PASS T2 (%d datagrams round-trip)\n", N);
    return 0;
}

/* T3: a datagram at the per-udp DGRAM_MAX bound (2048) round-trips (largest retained payload). */
#define T3_LEN 2048u
static unsigned char g_big[T3_LEN];
static int t3_max(KlEventCtx *ctx) {
    reset_capture();
    for (unsigned i = 0; i < T3_LEN; i++) g_big[i] = (unsigned char)(i * 31u + 7u);

    KlDatagram rx, tx;
    KlDatagramSocketConfig rc = { .ctx = ctx, .bind_addr = "127.0.0.1",
                       .recv_cap = 4096 };
    KlDatagramSocketConfig tc = { .ctx = ctx };
    if (kl_datagram_socket_init(&rx, &rc) != 0) return fail("rx init");
    if (kl_datagram_socket_init(&tx, &tc) != 0) { dg_close_free(ctx, &rx); return fail("tx init"); }
    if (kl_datagram_recv_start(&rx, on_recv, NULL) != 0) { dg_close_free(ctx, &tx); dg_close_free(ctx, &rx); return fail("recv_start"); }
    uint16_t port = kl_datagram_local_port(&rx);
    KlSockAddr dst; dest_v4(&dst, port);

    if (kl_datagram_send(&tx, &(KlDatagramMessage){ .data = g_big, .len = T3_LEN, .peer = &dst, .tos = -1 }) != KL_DATAGRAM_ACCEPTED) { dg_close_free(ctx, &tx); dg_close_free(ctx, &rx); return fail("T3 send"); }
    pump_until(ctx, 1, 400);
    int rc2 = 0;
    if (g_got != 1) rc2 = fail("T3: datagram not received");
    else if (g_len != T3_LEN || memcmp(g_buf, g_big, T3_LEN) != 0) rc2 = fail("T3: large payload mismatch");

    dg_close_free(ctx, &tx);
    dg_close_free(ctx, &rx);
    if (rc2) return rc2;
    printf("PASS T3 (max-size datagram round-trip, %u bytes)\n", T3_LEN);
    return 0;
}

/* T4 - close-with-armed-recv: deliver one datagram (which re-arms the recv), then free the rx socket
 * with a recv armed. Exercises the recv-side arm-ref release on close. Must not leak / crash. */
static int t4_close_with_armed_recv(KlEventCtx *ctx) {
    reset_capture();
    KlDatagram rx, tx;
    KlDatagramSocketConfig rc = { .ctx = ctx, .bind_addr = "127.0.0.1" };
    KlDatagramSocketConfig tc = { .ctx = ctx };
    if (kl_datagram_socket_init(&rx, &rc) != 0) return fail("rx init");
    if (kl_datagram_socket_init(&tx, &tc) != 0) { dg_close_free(ctx, &rx); return fail("tx init"); }
    if (kl_datagram_recv_start(&rx, on_recv, NULL) != 0) { dg_close_free(ctx, &tx); dg_close_free(ctx, &rx); return fail("recv_start"); }
    uint16_t port = kl_datagram_local_port(&rx);
    KlSockAddr dst; dest_v4(&dst, port);

    if (kl_datagram_send(&tx, &(KlDatagramMessage){ .data = "one", .len = 3, .peer = &dst, .tos = -1 }) != KL_DATAGRAM_ACCEPTED) { dg_close_free(ctx, &tx); dg_close_free(ctx, &rx); return fail("T4 send"); }
    pump_until(ctx, 1, 200);   /* deliver one; the datagram machine re-arms → a recv is armed at free time */

    /* Free with a recv armed (arm token ref outstanding): kl_lwr_udp_close releases it; no leak. */
    dg_close_free(ctx, &tx);
    dg_close_free(ctx, &rx);
    printf("PASS T4 (close-with-armed-recv, no leak/crash)\n");
    return 0;
}

/* T5 - close-with-undrained-sends: post several sends and free the SENDER before ANY drain, so its
 * pending KL_COMP_DGRAM_SEND records are released by kl_lwr_udp_close (each holds one stable-token
 * ref) rather than transferred out by the drain. T4 pumps between the sends and the free, and the
 * drain consumes ALL pending sends of a slot in one tick: so T4 exercises the send TRANSFER path,
 * NOT the send release-on-close path. This isolates the latter: no drain runs before dg_close_free(ctx, &tx),
 * so pend_send > 0 at close and the retained refs must be released there exactly once (ASan/LSan
 * gate). The trailing pump (AFTER the free) only lets lwIP process the queued loopback datagrams so
 * their pbufs are freed: it cannot touch the already-freed sender. */
static int t5_close_with_undrained_sends(KlEventCtx *ctx) {
    reset_capture();
    KlDatagram rx, tx;
    KlDatagramSocketConfig rc = { .ctx = ctx, .bind_addr = "127.0.0.1" };
    KlDatagramSocketConfig tc = { .ctx = ctx };
    if (kl_datagram_socket_init(&rx, &rc) != 0) return fail("T5 rx init");
    if (kl_datagram_socket_init(&tx, &tc) != 0) { dg_close_free(ctx, &rx); return fail("T5 tx init"); }
    if (kl_datagram_recv_start(&rx, on_recv, NULL) != 0) { dg_close_free(ctx, &tx); dg_close_free(ctx, &rx); return fail("T5 recv_start"); }
    uint16_t port = kl_datagram_local_port(&rx);
    KlSockAddr dst; dest_v4(&dst, port);

    /* Three sends, NO pump/drain between them or before the free: the tx slot keeps pend_send == 3,
     * each holding one token ref that only close can release. */
    for (int i = 0; i < 3; i++)
        if (kl_datagram_send(&tx, &(KlDatagramMessage){ .data = "snd", .len = 3, .peer = &dst, .tos = -1 }) != KL_DATAGRAM_ACCEPTED) { dg_close_free(ctx, &tx); dg_close_free(ctx, &rx); return fail("T5 send"); }

    dg_close_free(ctx, &tx);          /* releases the 3 undrained pending-send token refs (never transferred) */
    pump_until(ctx, 1, 200);   /* drain the loopback datagrams (frees their pbufs); tx is already gone */
    dg_close_free(ctx, &rx);
    printf("PASS T5 (close-with-undrained-sends, no leak/crash)\n");
    return 0;
}

/* T6: one-held-packet overflow, driven at the GLUE level (like T7).
 *
 * Through the public KlDatagram API the two datagrams cannot be forced into a SINGLE netif_poll:
 * KlDatagram posts each send as a completion op flushed DURING a loop tick (interleaved with the
 * lwIP poll), so back-to-back sends spread across ticks and the recv re-arms between them: both
 * would be delivered. A SYNCHRONOUS udp_sendto would land both before the first poll; the public
 * send has no such eager timing. The one-held-slot overflow is a BACKEND property,
 * deterministic only where the send is synchronous: the glue (kl_lwr_udp_send → udp_sendto). With a
 * recv armed, TWO datagrams delivered in one netif_poll must surface EXACTLY ONE RECV on drain (the
 * backend holds one, drops the second; a former 16-entry ring would have surfaced both). Sibling of
 * T7: same one-live-ctx glue harness, run sequentially after the event ctx is freed. */
static int t6_glue_one_held(void) {
    KlAllocator alloc = kl_allocator_default();
    void *lwrctx = kl_lwr_ctx_create(&alloc, 4);
    if (!lwrctx) return fail("T6: ctx create");
    void *loopif = kl_lwr_ctx_loopif(lwrctx);

    const uint8_t lo[4] = { 127, 0, 0, 1 };
    void *rx = kl_lwr_udp_new();
    void *tx = kl_lwr_udp_new();
    if (!rx || !tx || kl_lwr_udp_bind(rx, lo, 0) != 0) { kl_lwr_ctx_destroy(lwrctx); return fail("T6: rx bind"); }
    uint16_t port = kl_lwr_udp_local_port(rx);

    KlDgramLife *lrx = kl_dgram_life_create(&alloc, NULL, t7_noop_final, NULL, (KlDgramDispatchFn)0);
    KlDgramLife *ltx = kl_dgram_life_create(&alloc, NULL, t7_noop_final, NULL, (KlDgramDispatchFn)0);
    if (!lrx || !ltx) {
        kl_dgram_life_release(lrx); kl_dgram_life_release(ltx);
        kl_lwr_ctx_destroy(lwrctx); return fail("T6: token create");
    }

    KlLwrUdpRecord recs[8];
    int delivered = 0, held_ok = 1, stale = 0, rc = 0;

    /* Post a recv, then send TWO datagrams BEFORE any tick → both enter the SAME netif_poll. */
    if (kl_lwr_udp_post_recv(lwrctx, rx, lrx) != 0)                     rc = fail("T6: post_recv");
    else if (kl_lwr_udp_send(lwrctx, tx, ltx, "one", 3, lo, port) != 0) rc = fail("T6: send one");
    else if (kl_lwr_udp_send(lwrctx, tx, ltx, "two", 3, lo, port) != 0) rc = fail("T6: send two");
    if (rc == 0) {
        kl_lwr_lwip_tick(loopif);                       /* one netif_poll delivers both to udp_recv */
        int n1 = kl_lwr_udp_drain(lwrctx, recs, 8);     /* the held datagram (+ the two SENDs) surfaces */
        for (int i = 0; i < n1; i++) {
            if (recs[i].kind == KL_LWR_DGRAM_RECV && !recs[i].terminal) {
                delivered++;
                if (recs[i].len != 3 || memcmp(recs[i].data, "one", 3) != 0) held_ok = 0;
            }
            kl_dgram_life_release((KlDgramLife *)recs[i].life);
        }
        /* Re-arm + tick + drain: the dropped "two" was never held, so NOTHING stale surfaces. */
        if (kl_lwr_udp_post_recv(lwrctx, rx, lrx) != 0) rc = fail("T6: re-arm post_recv");
        else {
            kl_lwr_lwip_tick(loopif);
            int n2 = kl_lwr_udp_drain(lwrctx, recs, 8);
            for (int i = 0; i < n2; i++) {
                if (recs[i].kind == KL_LWR_DGRAM_RECV && !recs[i].terminal) stale++;
                kl_dgram_life_release((KlDgramLife *)recs[i].life);
            }
        }
    }

    kl_lwr_udp_close(lwrctx, rx);       /* releases any un-drained arm ref */
    kl_lwr_udp_close(lwrctx, tx);       /* releases any un-drained pending-send refs */
    kl_dgram_life_release(lrx);
    kl_dgram_life_release(ltx);
    kl_lwr_ctx_destroy(lwrctx);

    if (rc != 0)        return rc;
    if (delivered != 1) return fail("T6: expected exactly one delivery (the second must be dropped)");
    if (!held_ok)       return fail("T6: wrong held datagram (should be the first)");
    if (stale != 0)     return fail("T6: the dropped second datagram surfaced on re-arm (bad held slot)");
    printf("PASS T6 (glue: one-held overflow, second datagram dropped, exactly one delivered)\n");
    return 0;
}

/* T7: no stale capture while UNARMED, driven at the GLUE level.
 *
 * Through the public KlDatagram API this is not observable: KlDgramRecv's recv_stop is terminal (it never
 * re-posts after an unarmed gap) and the completion dispatch independently drops on recv_active == 0: so a
 * transport-level test cannot exhibit a stale delivery. One layer down the CONSUMER controls re-posting, which is where
 * the stale-buffering bug would bite: a datagram captured while no recv is armed would be surfaced on the
 * NEXT post_recv. This drives kl_lwr_udp_* directly to prove the recv callback drops it.
 *
 * Runs after the shared event ctx is freed (kl_lwr_ctx_create rejects a second CONCURRENT ctx). Manages
 * the stable tokens itself: each posted op retains one ref, the drain TRANSFERS it to the record (we
 * release it), and close releases any op ref not drained: so the suite stays LSan-clean. */
static int t7_glue_unarmed_drop(void) {
    KlAllocator alloc = kl_allocator_default();
    void *lwrctx = kl_lwr_ctx_create(&alloc, 4);
    if (!lwrctx) return fail("T7: ctx create");
    void *loopif = kl_lwr_ctx_loopif(lwrctx);

    const uint8_t lo[4] = { 127, 0, 0, 1 };
    void *rx = kl_lwr_udp_new();
    void *tx = kl_lwr_udp_new();
    if (!rx || !tx || kl_lwr_udp_bind(rx, lo, 0) != 0) { kl_lwr_ctx_destroy(lwrctx); return fail("T7: rx bind"); }
    uint16_t port = kl_lwr_udp_local_port(rx);

    KlDgramLife *lrx = kl_dgram_life_create(&alloc, NULL, t7_noop_final, NULL, (KlDgramDispatchFn)0);   /* owner ref each */
    KlDgramLife *ltx = kl_dgram_life_create(&alloc, NULL, t7_noop_final, NULL, (KlDgramDispatchFn)0);
    if (!lrx || !ltx) {                 /* release whichever succeeded (NULL-safe) before teardown */
        kl_dgram_life_release(lrx);
        kl_dgram_life_release(ltx);
        kl_lwr_ctx_destroy(lwrctx);
        return fail("T7: token create");
    }

    KlLwrUdpRecord recs[8];
    int got_recv1 = 0, recv2 = 0, send2 = 0, stale = 0;
    int rc = 0;

    /* (1) ARMED: post a recv, send "a", tick → recv callback captures it; drain surfaces one RECV
     * (+ the matching SEND) and CONSUMES the arm (rx is now UNARMED). */
    if (kl_lwr_udp_post_recv(lwrctx, rx, lrx) != 0)                rc = fail("T7: post_recv");
    else if (kl_lwr_udp_send(lwrctx, tx, ltx, "a", 1, lo, port) != 0) rc = fail("T7: send a");
    if (rc == 0) {
        kl_lwr_lwip_tick(loopif);
        int n1 = kl_lwr_udp_drain(lwrctx, recs, 8);
        for (int i = 0; i < n1; i++) {
            if (recs[i].kind == KL_LWR_DGRAM_RECV) got_recv1++;
            kl_dgram_life_release((KlDgramLife *)recs[i].life);   /* release each transferred ref */
        }

        /* (2) UNARMED: "b" MUST actually send: its SEND completion proves the datagram traversed the
         * loopback to the recv callback (else the step-(3) oracle would false-green). The callback must
         * DROP it, so this drain yields exactly one SEND and ZERO RECV. */
        if (kl_lwr_udp_send(lwrctx, tx, ltx, "b", 1, lo, port) != 0) rc = fail("T7: send b");
        else {
            kl_lwr_lwip_tick(loopif);
            int n2 = kl_lwr_udp_drain(lwrctx, recs, 8);
            for (int i = 0; i < n2; i++) {
                if (recs[i].kind == KL_LWR_DGRAM_RECV)      recv2++;
                else if (recs[i].kind == KL_LWR_DGRAM_SEND) send2++;
                kl_dgram_life_release((KlDgramLife *)recs[i].life);
            }

            /* (3) RE-ARM + drain: the re-post MUST succeed (else the drain trivially yields no RECV and
             * the oracle false-greens). A correctly-dropped "b" surfaces NOTHING here; a buggily-held
             * "b" surfaces as a stale RECV. */
            if (kl_lwr_udp_post_recv(lwrctx, rx, lrx) != 0) rc = fail("T7: re-arm post_recv");
            else {
                int n3 = kl_lwr_udp_drain(lwrctx, recs, 8);
                for (int i = 0; i < n3; i++) {
                    if (recs[i].kind == KL_LWR_DGRAM_RECV) stale++;
                    kl_dgram_life_release((KlDgramLife *)recs[i].life);
                }
            }
        }
    }

    kl_lwr_udp_close(lwrctx, rx);       /* releases any un-drained arm ref */
    kl_lwr_udp_close(lwrctx, tx);       /* releases any un-drained pending-send refs */
    kl_dgram_life_release(lrx);         /* owner refs → final release runs t7_noop_final */
    kl_dgram_life_release(ltx);
    kl_lwr_ctx_destroy(lwrctx);

    if (rc != 0)        return rc;
    if (got_recv1 != 1) return fail("T7: setup, the armed datagram was not delivered");
    if (send2 != 1)     return fail("T7: the unarmed datagram 'b' never sent, oracle would false-green");
    if (recv2 != 0)     return fail("T7: 'b' surfaced as a RECV while unarmed");
    if (stale != 0)     return fail("T7: a datagram captured while UNARMED surfaced on re-arm (stale buffering)");
    printf("PASS T7 (glue: unarmed datagram dropped, 'b' sent but never delivered, no stale on re-arm)\n");
    return 0;
}

int main(void) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    if (kl_event_ctx_init_ex(&ctx, &alloc, kl_event_provider_lwip_raw()) != 0)
        return fail("ctx init (bring up lwIP raw)");
    /* Wire the ctx's sockets to the raw provider: the same auto-wire a KlHttpServer does via the
     * loop's native_provider(). A standalone KlEventCtx leaves ctx.sockets NULL (POSIX default),
     * so a UDP consumer (like this test / the future DNS resolver) must set it explicitly. */
    ctx.sockets = kl_socket_provider_lwip_raw();

    int rc = 0;
    if (rc == 0) rc = t1_single(&ctx);
    if (rc == 0) rc = t2_burst(&ctx);
    if (rc == 0) rc = t3_max(&ctx);
    if (rc == 0) rc = t4_close_with_armed_recv(&ctx);
    if (rc == 0) rc = t5_close_with_undrained_sends(&ctx);

    kl_event_ctx_free(&ctx);   /* free the event ctx FIRST: the raw backend allows only one live ctx */
    if (rc != 0) return 1;

    /* T6 + T7 stand up their OWN standalone glue ctxs (sequential: the event ctx is now gone).
     * Both are BACKEND-property tests needing synchronous, controllable send timing at the glue seam. */
    if (t6_glue_one_held() != 0)   return 1;
    if (t7_glue_unarmed_drop() != 0) return 1;

    printf("LC-3a PASS\n");
    return 0;
}
