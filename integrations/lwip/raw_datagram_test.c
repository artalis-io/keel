/*
 * raw_datagram_test.c — the PUBLIC KlDatagram over the lwIP-raw COMPLETION backend (7B-8).
 *
 * The live lwIP-raw binding of the public facade, the completion counterpart of pollcomp/io_uring
 * (7B-4/7B-5). Brings up NO_SYS=1 lwIP + loopback via kl_event_provider_lwip_raw(), preps datagram fds
 * through the raw socket provider (the KlDatagram contract), and exercises:
 *   T1  send→recv roundtrip + a clean graceful close → DETACHED.
 *   T2  empty armed cancellation: recv armed but NO datagram → close still retires the armed recv (the
 *         7B-8 cancel→terminal path) → DETACHED.
 *   T3  glue-level cancel semantics (kl_lwr_udp_cancel_recv/drain/recv_pending directly): PENDING while a
 *         terminal is queued → RETIRED after it drains; repeated cancellation is idempotent; the drain
 *         emits exactly ONE terminal (no duplicate); ref accounting is LSan-clean.
 *
 * The 7B-8 glue fix (a cancelled armed recv → a context-owned pending terminal the drain surfaces as an
 * ok=0 completion) is what lets the KlDatagram close coordinator retire recv_inflight; the removed UDP object never used
 * cancel_dgram, so its path (raw_udp_test T1-T7) is unchanged.
 */

#include <keel/datagram.h>
#include <keel/datagram_detail.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/sockaddr.h>

#include "keel_lwip_raw.h"  /* kl_event_provider_lwip_raw / kl_socket_provider_lwip_raw */
#include "lwip_raw_glue.h"  /* T3 drives the glue directly (kl_lwr_udp_* / kl_lwr_ctx_*) */
#include "../../src/socket.h"  /* kl_sock_socket / _bind / _set_nonblocking / _get_local_addr / _close */

#include <stdio.h>
#include <string.h>

/* KlDgramLife API (forward-declared to drive the glue directly, as raw_udp_test does; resolves against
 * libkeel's datagram_life.o without pulling the internal src/ header). */
typedef struct KlDgramLife KlDgramLife;
struct KlCompletionEvent;
typedef void (*KlDgramDispatchFn)(void *target, const struct KlCompletionEvent *ev);
KlDgramLife *kl_dgram_life_create(KlAllocator *alloc, void *target, void (*on_final)(void *), void *final_ctx,
                                  KlDgramDispatchFn dispatch);
void         kl_dgram_life_release(KlDgramLife *l);

static int fail(const char *msg) { printf("7B-8 FAIL: %s\n", msg); return 1; }

static int    g_got;
static char   g_buf[2048];
static size_t g_len;
static int    g_has_peer;
static void on_recv(void *ud, const void *data, size_t len, const KlSockAddr *peer,
                    const KlSockAddr *local, unsigned flags) {
    (void)ud; (void)local; (void)flags;
    if (len > sizeof(g_buf)) len = sizeof(g_buf);
    memcpy(g_buf, data, len);
    g_len = len; g_has_peer = (peer != NULL); g_got++;
}

static void pump_until_got(KlEventCtx *ctx, int want, int ticks) {
    for (int i = 0; i < ticks && g_got < want; i++) kl_event_ctx_run(ctx, 16, 5);
}
static void pump_close(KlEventCtx *ctx, KlDatagram *dg, int ticks) {
    for (int i = 0; i < ticks && kl_datagram_close_state(dg) != KL_DGRAM_CLOSE_CLOSED; i++)
        kl_event_ctx_run(ctx, 16, 5);
}

static KlSocketHandle prep_fd(const KlSocketProvider *sp, int do_bind, uint16_t *out_port) {
    KlSocketHandle fd = kl_sock_socket(sp, AF_INET, SOCK_DGRAM, 0);
    if (!kl_handle_valid(fd)) return fd;
    kl_sock_set_nonblocking(sp, fd);
    KlDatagramSocketConfig ucfg; memset(&ucfg, 0, sizeof(ucfg));
    if (sp && sp->dgram && sp->dgram->configure) (void)sp->dgram->configure(sp->context, fd, AF_INET, &ucfg);
    if (do_bind) {
        KlSockAddr b; kl_sockaddr_parse(&b, "127.0.0.1", 0);
        if (kl_sock_bind(sp, fd, &b) != 0) { kl_sock_close(sp, fd); return KL_INVALID_SOCKET; }
        KlSockAddr local;
        if (kl_sock_get_local_addr(sp, fd, &local) != 0) { kl_sock_close(sp, fd); return KL_INVALID_SOCKET; }
        *out_port = kl_sockaddr_port(&local);
    }
    return fd;
}

/* T1 — roundtrip + clean DETACHED close. */
static int t1_roundtrip(KlEventCtx *ctx, const KlSocketProvider *sp) {
    g_got = 0; g_len = 0; g_has_peer = 0;
    uint16_t port = 0;
    KlSocketHandle rxfd = prep_fd(sp, 1, &port);
    KlSocketHandle txfd = prep_fd(sp, 0, NULL);
    if (!kl_handle_valid(rxfd) || !kl_handle_valid(txfd) || port == 0) return fail("T1 fd prep");

    KlDatagram rx, tx; memset(&rx, 0, sizeof(rx)); memset(&tx, 0, sizeof(tx));
    KlDatagramConfig rc = { .ctx = ctx, .alloc = ctx->alloc, .sockets = sp, .fd = rxfd,
                            .send_slots = 4, .send_slot_cap = 1500, .recv_cap = 2048 };
    KlDatagramConfig tc = rc; tc.fd = txfd;
    if (kl_datagram_init(&rx, &rc) != 0) return fail("T1 rx init");
    if (kl_datagram_init(&tx, &tc) != 0) { kl_datagram_close_cancel(&rx); pump_close(ctx,&rx,50); kl_datagram_free(&rx); return fail("T1 tx init"); }
    if (kl_datagram_recv_start(&rx, on_recv, NULL) != 0) return fail("T1 recv_start");

    KlSockAddr dst; const uint8_t lo[4] = { 127, 0, 0, 1 }; kl_sockaddr_from_ipv4(&dst, lo, port);
    const char *msg = "keel-datagram-lwip-raw";
    KlDatagramMessage m = { .data = msg, .len = strlen(msg), .peer = &dst, .tos = -1 };
    if (kl_datagram_send(&tx, &m) != KL_DATAGRAM_ACCEPTED) return fail("T1 send refused");

    pump_until_got(ctx, 1, 200);
    if (g_got != 1 || g_len != strlen(msg) || memcmp(g_buf, msg, g_len) != 0 || !g_has_peer) return fail("T1 roundtrip");

    if (kl_datagram_close_begin(&rx) != 0) return fail("T1 rx close_begin");
    pump_close(ctx, &rx, 200);
    if (kl_datagram_close_state(&rx) != KL_DGRAM_CLOSE_CLOSED ||
        kl_datagram_close_result(&rx) != KL_DGRAM_DETACHED) return fail("T1 rx not DETACHED");
    kl_datagram_close_begin(&tx); pump_close(ctx, &tx, 200);
    if (kl_datagram_close_state(&tx) != KL_DGRAM_CLOSE_CLOSED) return fail("T1 tx not CLOSED");
    kl_datagram_free(&rx); kl_datagram_free(&tx);
    return 0;
}

/* T2 — empty armed cancellation: a recv is armed but no datagram ever arrives; graceful close must still
 * retire the armed recv (via the 7B-8 cancel→terminal path) and reach DETACHED. */
static int t2_empty_armed_cancel(KlEventCtx *ctx, const KlSocketProvider *sp) {
    g_got = 0;
    uint16_t port = 0;
    KlSocketHandle rxfd = prep_fd(sp, 1, &port);
    if (!kl_handle_valid(rxfd) || port == 0) return fail("T2 fd prep");
    KlDatagram rx; memset(&rx, 0, sizeof(rx));
    KlDatagramConfig rc = { .ctx = ctx, .alloc = ctx->alloc, .sockets = sp, .fd = rxfd,
                            .send_slots = 2, .send_slot_cap = 1500, .recv_cap = 2048 };
    if (kl_datagram_init(&rx, &rc) != 0) return fail("T2 init");
    if (kl_datagram_recv_start(&rx, on_recv, NULL) != 0) return fail("T2 recv_start");  /* armed, no datagram */
    if (kl_datagram_close_begin(&rx) != 0) return fail("T2 close_begin");
    pump_close(ctx, &rx, 200);
    if (kl_datagram_close_state(&rx) != KL_DGRAM_CLOSE_CLOSED ||
        kl_datagram_close_result(&rx) != KL_DGRAM_DETACHED) return fail("T2 empty-armed not DETACHED");
    if (g_got != 0) return fail("T2 spurious delivery");   /* terminal must NOT deliver a datagram */
    kl_datagram_free(&rx);
    return 0;
}

static void noop_final(void *ctx) { (void)ctx; }

/* T3 — glue-level cancel semantics (own glue ctx, after the event ctx is gone; the raw backend allows
 * only one live ctx). Proves PENDING→RETIRED, idempotent repeated cancel, and no duplicate terminal. */
static int t3_glue_cancel_semantics(void) {
    KlAllocator alloc = kl_allocator_default();
    void *lwrctx = kl_lwr_ctx_create(&alloc, 4);
    if (!lwrctx) return fail("T3 ctx");
    const uint8_t lo[4] = { 127, 0, 0, 1 };
    void *rx = kl_lwr_udp_new();
    if (!rx || kl_lwr_udp_bind(rx, lo, 0) != 0) { kl_lwr_ctx_destroy(lwrctx); return fail("T3 bind"); }
    KlDgramLife *l = kl_dgram_life_create(&alloc, NULL, noop_final, NULL, (KlDgramDispatchFn)0);
    if (!l) { kl_lwr_ctx_destroy(lwrctx); return fail("T3 token"); }

    int rc = 0;
    KlLwrUdpRecord recs[8];
    /* Arm a recv (takes a ref: owner + arm). No datagram. */
    if (kl_lwr_udp_post_recv(lwrctx, rx, l) != 0) rc = fail("T3 post_recv");
    if (rc == 0 && kl_lwr_udp_recv_pending(lwrctx, l) != 0) rc = fail("T3 pending before cancel");
    /* Cancel → moves the arm to a pending terminal (PENDING). */
    if (rc == 0) { kl_lwr_udp_cancel_recv(lwrctx, l);
                   if (kl_lwr_udp_recv_pending(lwrctx, l) != 1) rc = fail("T3 not PENDING after cancel"); }
    /* Repeated cancel is idempotent — no second terminal queued. */
    if (rc == 0) { kl_lwr_udp_cancel_recv(lwrctx, l);
                   if (kl_lwr_udp_recv_pending(lwrctx, l) != 1) rc = fail("T3 repeated cancel changed state"); }
    /* Drain → exactly ONE terminal (ok=0, terminal=1); RETIRED after. */
    if (rc == 0) {
        int n = kl_lwr_udp_drain(lwrctx, recs, 8);
        int term = 0;
        for (int i = 0; i < n; i++) {
            if (recs[i].kind == KL_LWR_DGRAM_RECV && recs[i].terminal) term++;
            kl_dgram_life_release((KlDgramLife *)recs[i].life);   /* release each transferred ref */
        }
        if (term != 1) rc = fail("T3 not exactly one terminal");
        else if (kl_lwr_udp_recv_pending(lwrctx, l) != 0) rc = fail("T3 not RETIRED after drain");
        else {
            int n2 = kl_lwr_udp_drain(lwrctx, recs, 8);   /* no duplicate */
            for (int i = 0; i < n2; i++) kl_dgram_life_release((KlDgramLife *)recs[i].life);
            if (n2 != 0) rc = fail("T3 duplicate terminal");
        }
    }
    kl_lwr_udp_close(lwrctx, rx);          /* nothing left to release (arm cancelled, terminal drained) */
    kl_dgram_life_release(l);              /* drop the owner ref → token freed (LSan-clean) */
    kl_lwr_ctx_destroy(lwrctx);
    return rc;
}

/* T4 — bounded-state saturation/reuse (review): arm+cancel+close EVERY udp slot so all pending terminals
 * are queued (each tied 1:1 to its slot index); no slot may be reused until the terminals drain; after
 * draining, reuse works; exact ref accounting (each life: owner + arm→terminal→drain-release). */
static int t4_saturation_reuse(void) {
    KlAllocator alloc = kl_allocator_default();
    void *lwrctx = kl_lwr_ctx_create(&alloc, 4);
    if (!lwrctx) return fail("T4 ctx");
    const uint8_t lo[4] = { 127, 0, 0, 1 };
    KlDgramLife *life[16]; int n = 0; int rc = 0;

    /* Fill every udp slot: new+bind+arm+cancel (→ pending terminal on that slot) + close (frees the pcb
     * slot; the terminal stays queued and pins the slot against reuse). */
    for (;;) {
        void *p = kl_lwr_udp_new();
        if (!p) break;                          /* slot table full */
        if (n >= 16) { rc = fail("T4 slot count > 16"); break; }
        KlDgramLife *l = kl_dgram_life_create(&alloc, NULL, noop_final, NULL, (KlDgramDispatchFn)0);
        if (!l || kl_lwr_udp_bind(p, lo, 0) != 0 || kl_lwr_udp_post_recv(lwrctx, p, l) != 0) {
            kl_dgram_life_release(l); kl_lwr_udp_close(lwrctx, p); rc = fail("T4 arm"); break;
        }
        life[n++] = l;
        kl_lwr_udp_cancel_recv(lwrctx, l);      /* → pending terminal tied to this slot */
        if (kl_lwr_udp_recv_pending(lwrctx, l) != 1) { rc = fail("T4 not PENDING"); break; }
        kl_lwr_udp_close(lwrctx, p);            /* free the pcb slot; terminal remains, pins the slot */
    }
    if (rc == 0 && n < 2) rc = fail("T4 too few slots to test");
    /* No slot is reusable while terminals are pending. */
    if (rc == 0 && kl_lwr_udp_new() != NULL) rc = fail("T4 slot reused before drain");
    /* Drain → exactly n terminals; release each transferred ref. */
    if (rc == 0) {
        KlLwrUdpRecord recs[16]; int drained = 0;
        for (int pass = 0; pass < 8 && drained < n; pass++) {
            int d = kl_lwr_udp_drain(lwrctx, recs, 16);
            for (int i = 0; i < d; i++) {
                if (recs[i].kind == KL_LWR_DGRAM_RECV && recs[i].terminal) drained++;
                kl_dgram_life_release((KlDgramLife *)recs[i].life);
            }
            if (d == 0) break;
        }
        if (drained != n) rc = fail("T4 terminal count != slots");
    }
    /* Reuse works now that the terminals drained. */
    if (rc == 0) {
        void *p = kl_lwr_udp_new();
        if (!p) rc = fail("T4 no reuse after drain");
        else kl_lwr_udp_close(lwrctx, p);
    }
    for (int i = 0; i < n; i++) kl_dgram_life_release(life[i]);   /* drop each owner ref → freed */
    kl_lwr_ctx_destroy(lwrctx);
    return rc;
}

int main(void) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    if (kl_event_ctx_init_ex(&ctx, &alloc, kl_event_provider_lwip_raw()) != 0) return fail("ctx init");
    ctx.sockets = kl_socket_provider_lwip_raw();
    const KlSocketProvider *sp = ctx.sockets;

    int rc = 0;
    if (rc == 0) rc = t1_roundtrip(&ctx, sp);
    if (rc == 0) rc = t2_empty_armed_cancel(&ctx, sp);
    kl_event_ctx_free(&ctx);   /* free the event ctx FIRST — one live ctx at a time */
    if (rc != 0) return 1;

    if (t3_glue_cancel_semantics() != 0) return 1;
    if (t4_saturation_reuse() != 0) return 1;

    printf("7B-8 PASS (roundtrip + empty-armed cancel + PENDING->RETIRED + saturation/reuse over lwIP-raw)\n");
    return 0;
}
