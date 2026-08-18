/*
 * test_dgram_close.c — Phase B step 4: close/cancel + confirmed-detachment over send + recv.
 *
 * Covers: both retirement orders, graceful drain-then-detach, abortive discard + sync/async cancel,
 * cancel-at-most-once, graceful→abortive escalation, error during graceful drain (no permanent
 * wait), close/cancel re-entered from a cancel hook, exactly-once destructive on_close, and free
 * refused before confirmed detachment.
 */
#include "utest.h"

#include "../src/datagram_close.h"

#include <string.h>

/* ── send/recv completion mocks ──────────────────────────────────────────────────────────────── */
static KlDgramSubmitResult send_submit(void *ctx, const void *data, size_t len,
                                       const KlSockAddr *peer, const KlSockAddr *local, int tos) {
    (void)ctx; (void)data; (void)len; (void)peer; (void)local; (void)tos;
    return KL_DGRAM_SUBMIT_INFLIGHT;
}
static int recv_arm(void *ctx) { (void)ctx; return 0; }   /* post; no inline completion */
static void recv_deliver(void *ctx, const void *d, size_t n,
                         const KlSockAddr *p, const KlSockAddr *l, unsigned f) {
    (void)ctx; (void)d; (void)n; (void)p; (void)l; (void)f;
}

/* ── close-side globals + cancel hooks ───────────────────────────────────────────────────────── */
static KlDgramSend *g_send;
static KlDgramRecv *g_recv;
static KlDgramClose *g_close;
static int g_on_close, g_cancel_send_calls, g_cancel_recv_calls;
enum { CANCEL_ASYNC = 0, CANCEL_SYNC, CANCEL_REENTER };
static int g_cancel_mode;

static void cancel_send(void *ctx) {
    (void)ctx; g_cancel_send_calls++;
    if (g_cancel_mode == CANCEL_SYNC)   kl_dgram_send_on_complete(g_send, 0);   /* inline retire */
    else if (g_cancel_mode == CANCEL_REENTER) kl_dgram_close_cancel(g_close);   /* reenter close */
}
static void cancel_recv(void *ctx) {
    (void)ctx; g_cancel_recv_calls++;
    if (g_cancel_mode == CANCEL_SYNC)   kl_dgram_recv_on_complete(g_recv, 0, 0);/* inline retire */
    else if (g_cancel_mode == CANCEL_REENTER) kl_dgram_close_cancel(g_close);
}
static KlDatagramCloseResult g_on_close_result;
static void on_close_cb(void *ctx, KlDatagramCloseResult result) { (void)ctx; g_on_close++; g_on_close_result = result; }

/* callback-initiated close: a machine callback calls close; detachment must be DEFERRED to the
 * outermost frame's unwind — otherwise the callback's frame would touch freed state (ASan). */
static KlDgramClose *g_cb_close;
static int           g_cb_did;
static void cb_do_close(void) { if (!g_cb_did) { g_cb_did = 1; kl_dgram_close_begin(g_cb_close); } }
static void closing_deliver(void *c, const void *d, size_t n,
                            const KlSockAddr *p, const KlSockAddr *l, unsigned f) {
    (void)c; (void)d; (void)n; (void)p; (void)l; (void)f; cb_do_close();
}
static void closing_writable(void *c) { (void)c; cb_do_close(); }
static void closing_drain(void *c)    { (void)c; cb_do_close(); }
static KlDgramRecv  *g_ia_recv;
static KlDgramInbound *g_ia_slots;
static int inline_close_arm(void *ctx) {   /* posts + inline-completes into a closing delivery */
    (void)ctx;
    KlDgramSlot *in = kl_dgram_inbound_slot(g_ia_slots);
    kl_sockaddr_from_ipv4(&in->peer, (const unsigned char *)"\x7f\0\0\1", 53);   /* mandatory peer */
    memcpy(in->data, "I", 1);
    kl_dgram_recv_on_complete(g_ia_recv, 1, 1);
    return 0;
}

/* readiness direct-send submit hook that reentrantly requests close, then returns a chosen result.
 * The busy frame around the direct submit must defer detachment until kl_dgram_send returns. */
static int g_rc_result, g_rc_did;
static KlDgramSubmitResult reentrant_close_submit(void *ctx, const void *d, size_t n,
                                                  const KlSockAddr *p, const KlSockAddr *l, int t) {
    (void)ctx; (void)d; (void)n; (void)p; (void)l; (void)t;
    if (!g_rc_did) { g_rc_did = 1; kl_dgram_close_begin(g_cb_close); }
    return (KlDgramSubmitResult)g_rc_result;
}
/* Drive one reentrant-close-from-submit case; report observable state via out-params (ASSERT-free). */
static void run_reentrant_close(int submit_result, int *st, int *queued, int *detached, int *closes) {
    KlDgramSlots slots; KlAllocator a = kl_allocator_default();
    kl_dgram_slots_init(&slots, &a, 4, 64);
    KlDgramSend send;
    kl_dgram_send_init(&send, &slots, &a, /*completion*/0, KL_DGRAM_CAP_CONNECTED,
                       0, reentrant_close_submit, NULL);
    KlDgramClose close;
    kl_dgram_close_init(&close, &send, NULL, on_close_cb, NULL);
    g_cb_close = &close; g_rc_did = 0; g_rc_result = submit_result; g_on_close = 0;
    KlSockAddr p; kl_sockaddr_from_ipv4(&p, (const unsigned char *)"\x7f\0\0\1", 53);
    KlDatagramMessage m = { .data = "x", .len = 1, .peer = &p, .tos = -1 };
    *st       = (int)kl_dgram_send(&send, &m);          /* hook closes reentrantly, mid-frame */
    *queued   = (int)kl_dgram_send_queued(&send);
    *detached = kl_dgram_close_is_detached(&close);     /* must be 1 — deferred to send's final leave */
    *closes   = g_on_close;
    kl_dgram_close_free(&close);
    kl_dgram_send_free(&send);
    kl_dgram_slots_free(&slots);
}

/* Fixtures: slots + a completion send + a completion recv, wired into a close coordinator. */
typedef struct { KlDgramSlots slots; KlDgramInbound inbound; KlDgramSend send; KlDgramRecv recv; KlDgramClose close; } Fix;
static KlAllocator g_a;
static void fix_init(Fix *f, unsigned caps) {   /* fixed valid args → each init returns 0 */
    g_a = kl_allocator_default();
    kl_dgram_slots_init(&f->slots, &g_a, 4, 64);        /* object-owned outbound (send) */
    kl_dgram_inbound_init(&f->inbound, &g_a, 64);       /* life-ownable inbound (recv) */
    kl_dgram_send_init(&f->send, &f->slots, &g_a, /*completion*/1, caps, 0, send_submit, NULL);
    kl_dgram_recv_init(&f->recv, &f->inbound, /*completion*/1, recv_deliver, NULL,
                       recv_arm, NULL, NULL, NULL);
    kl_dgram_close_init(&f->close, &f->send, &f->recv, on_close_cb, NULL);
    g_send = &f->send; g_recv = &f->recv; g_close = &f->close;
    g_on_close = g_cancel_send_calls = g_cancel_recv_calls = 0;
}
static void fix_free(Fix *f) {   /* only when nothing is in flight */
    kl_dgram_close_free(&f->close);
    kl_dgram_recv_free(&f->recv);
    kl_dgram_send_free(&f->send);
    kl_dgram_slots_free(&f->slots);
    kl_dgram_inbound_free(&f->inbound);
}
static void send_one(Fix *f) {
    KlSockAddr p; kl_sockaddr_from_ipv4(&p, (const unsigned char *)"\x7f\0\0\1", 53);
    KlDatagramMessage m = { .data = "x", .len = 1, .peer = &p, .tos = -1 };
    (void)kl_dgram_send(&f->send, &m);   /* ACCEPTED for valid args */
}

/* graceful drain-then-detach — retire order send-then-recv */
UTEST(dgram_close, graceful_send_then_recv) {
    Fix f; fix_init(&f, KL_DGRAM_CAP_CONNECTED);
    send_one(&f);                                          /* send in flight */
    ASSERT_EQ(kl_dgram_recv_start(&f.recv), 0);            /* recv in flight */
    ASSERT_EQ(kl_dgram_close_begin(&f.close), 0);
    ASSERT_EQ(kl_dgram_close_is_detached(&f.close), 0);    /* both in flight */

    ASSERT_EQ(kl_dgram_send_on_complete(&f.send, 1), 0);   /* send retires, queue empty */
    ASSERT_EQ(kl_dgram_close_is_detached(&f.close), 0);    /* recv still in flight */
    ASSERT_EQ(kl_dgram_recv_on_complete(&f.recv, 0, 0), 0);/* recv retires (stopped-drop) */
    ASSERT_EQ(kl_dgram_close_is_detached(&f.close), 1);
    ASSERT_EQ(g_on_close, 1);
    fix_free(&f);
}

/* graceful — retire order recv-then-send */
UTEST(dgram_close, graceful_recv_then_send) {
    Fix f; fix_init(&f, KL_DGRAM_CAP_CONNECTED);
    send_one(&f);
    ASSERT_EQ(kl_dgram_recv_start(&f.recv), 0);
    ASSERT_EQ(kl_dgram_close_begin(&f.close), 0);
    ASSERT_EQ(kl_dgram_recv_on_complete(&f.recv, 0, 0), 0);
    ASSERT_EQ(kl_dgram_close_is_detached(&f.close), 0);    /* send still in flight */
    ASSERT_EQ(kl_dgram_send_on_complete(&f.send, 1), 0);
    ASSERT_EQ(kl_dgram_close_is_detached(&f.close), 1);
    ASSERT_EQ(g_on_close, 1);
    fix_free(&f);
}

/* graceful waits for the queued (unsubmitted) datagram to drain before detaching */
UTEST(dgram_close, graceful_drains_queue) {
    Fix f; fix_init(&f, KL_DGRAM_CAP_CONNECTED);
    send_one(&f);                                          /* A: in flight */
    send_one(&f);                                          /* B: queued (single-flight) */
    ASSERT_EQ((int)kl_dgram_send_queued(&f.send), 2);
    ASSERT_EQ(kl_dgram_close_begin(&f.close), 0);          /* recv NULL-less: recv started? none — */
    ASSERT_EQ(kl_dgram_close_is_detached(&f.close), 0);
    ASSERT_EQ(kl_dgram_send_on_complete(&f.send, 1), 0);   /* A retires; B pumped (in flight) */
    ASSERT_EQ((int)kl_dgram_send_queued(&f.send), 1);
    ASSERT_EQ(kl_dgram_close_is_detached(&f.close), 0);    /* queue not empty */
    ASSERT_EQ(kl_dgram_send_on_complete(&f.send, 1), 0);   /* B retires; queue empty */
    ASSERT_EQ(kl_dgram_close_is_detached(&f.close), 1);    /* recv never started → recv side is idle */
    ASSERT_EQ(g_on_close, 1);
    fix_free(&f);
}

/* abortive cancel with SYNCHRONOUS retirement: discard queued + cancel-once + detach exactly once */
UTEST(dgram_close, abortive_cancel_sync) {
    Fix f; fix_init(&f, KL_DGRAM_CAP_CONNECTED);
    ASSERT_EQ(kl_dgram_close_set_cancel(&f.close, cancel_recv, cancel_send, NULL), 0);
    send_one(&f);                                          /* A in flight */
    send_one(&f);                                          /* B queued */
    ASSERT_EQ(kl_dgram_recv_start(&f.recv), 0);
    g_cancel_mode = CANCEL_SYNC;
    ASSERT_EQ(kl_dgram_close_cancel(&f.close), 0);
    ASSERT_EQ(g_cancel_send_calls, 1);                     /* cancel-once */
    ASSERT_EQ(g_cancel_recv_calls, 1);
    ASSERT_EQ((int)kl_dgram_send_queued(&f.send), 0);      /* B (queued) discarded; A retired by cancel */
    ASSERT_EQ(kl_dgram_close_is_detached(&f.close), 1);
    ASSERT_EQ(g_on_close, 1);
    fix_free(&f);
}

/* abortive cancel with ASYNCHRONOUS retirement: cancels requested, detach only on real completion */
UTEST(dgram_close, abortive_cancel_async) {
    Fix f; fix_init(&f, KL_DGRAM_CAP_CONNECTED);
    ASSERT_EQ(kl_dgram_close_set_cancel(&f.close, cancel_recv, cancel_send, NULL), 0);
    send_one(&f);
    ASSERT_EQ(kl_dgram_recv_start(&f.recv), 0);
    g_cancel_mode = CANCEL_ASYNC;
    ASSERT_EQ(kl_dgram_close_cancel(&f.close), 0);
    ASSERT_EQ(g_cancel_send_calls, 1);
    ASSERT_EQ(g_cancel_recv_calls, 1);
    ASSERT_EQ(kl_dgram_close_is_detached(&f.close), 0);    /* ops still physically outstanding */
    ASSERT_EQ(kl_dgram_send_on_complete(&f.send, 0), -1);  /* real terminal completions */
    ASSERT_EQ(kl_dgram_close_is_detached(&f.close), 0);
    ASSERT_EQ(kl_dgram_recv_on_complete(&f.recv, 0, 0), 0);
    ASSERT_EQ(kl_dgram_close_is_detached(&f.close), 1);
    ASSERT_EQ(g_on_close, 1);
    fix_free(&f);
}

/* repeated cancel issues each cancel hook at most once */
UTEST(dgram_close, repeated_cancel_once) {
    Fix f; fix_init(&f, KL_DGRAM_CAP_CONNECTED);
    ASSERT_EQ(kl_dgram_close_set_cancel(&f.close, cancel_recv, cancel_send, NULL), 0);
    send_one(&f);
    ASSERT_EQ(kl_dgram_recv_start(&f.recv), 0);
    g_cancel_mode = CANCEL_ASYNC;
    ASSERT_EQ(kl_dgram_close_cancel(&f.close), 0);
    ASSERT_EQ(kl_dgram_close_cancel(&f.close), 0);         /* repeated — no re-invocation */
    ASSERT_EQ(g_cancel_send_calls, 1);
    ASSERT_EQ(g_cancel_recv_calls, 1);
    kl_dgram_send_on_complete(&f.send, 0);
    kl_dgram_recv_on_complete(&f.recv, 0, 0);
    ASSERT_EQ(g_on_close, 1);
    fix_free(&f);
}

/* graceful close may escalate to abortive cancel */
UTEST(dgram_close, graceful_to_abortive_escalation) {
    Fix f; fix_init(&f, KL_DGRAM_CAP_CONNECTED);
    ASSERT_EQ(kl_dgram_close_set_cancel(&f.close, cancel_recv, cancel_send, NULL), 0);
    send_one(&f); send_one(&f);                            /* A in flight, B queued */
    ASSERT_EQ(kl_dgram_recv_start(&f.recv), 0);
    ASSERT_EQ(kl_dgram_close_begin(&f.close), 0);          /* graceful — waits */
    ASSERT_EQ(kl_dgram_close_is_detached(&f.close), 0);
    g_cancel_mode = CANCEL_SYNC;
    ASSERT_EQ(kl_dgram_close_cancel(&f.close), 0);         /* escalate: discard B + cancel A + recv */
    ASSERT_EQ((int)kl_dgram_send_queued(&f.send), 0);
    ASSERT_EQ(kl_dgram_close_is_detached(&f.close), 1);
    ASSERT_EQ(g_on_close, 1);
    fix_free(&f);
}

/* ASYNC send failure during graceful drain escalates to abortive cleanup: discard queue, detach
 * only with the queue EMPTY (no permanent wait, no implicit slot ownership). */
UTEST(dgram_close, error_async_completion_during_graceful) {
    Fix f; fix_init(&f, KL_DGRAM_CAP_CONNECTED);
    send_one(&f);                                          /* A in flight */
    send_one(&f);                                          /* B queued (undrainable after the error) */
    ASSERT_EQ(kl_dgram_close_begin(&f.close), 0);
    ASSERT_EQ(kl_dgram_close_is_detached(&f.close), 0);
    ASSERT_EQ(kl_dgram_send_on_complete(&f.send, 0), -1);  /* A fails → sticky error; escalate */
    ASSERT_TRUE(kl_dgram_send_error(&f.send) != 0);
    ASSERT_EQ((int)kl_dgram_send_queued(&f.send), 0);      /* B discarded by escalation → queue empty */
    ASSERT_EQ(kl_dgram_close_is_detached(&f.close), 1);    /* detached only with the queue empty */
    ASSERT_EQ(g_on_close, 1);
    fix_free(&f);
}

/* SYNCHRONOUS submission failure with extra queued packets: err is set at send() time; a graceful
 * close escalates and discards the retained queue, then detaches. */
static KlDgramSubmitResult send_submit_err(void *ctx, const void *d, size_t n,
                                           const KlSockAddr *p, const KlSockAddr *l, int t) {
    (void)ctx; (void)d; (void)n; (void)p; (void)l; (void)t;
    return KL_DGRAM_SUBMIT_ERROR;                          /* synchronous submission failure */
}
UTEST(dgram_close, error_sync_submission_during_graceful) {
    KlDgramSlots slots; KlAllocator a = kl_allocator_default();
    ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64), 0);
    KlDgramSend send;
    ASSERT_EQ(kl_dgram_send_init(&send, &slots, &a, 1, KL_DGRAM_CAP_CONNECTED, 0, send_submit_err, NULL), 0);
    KlDgramClose close;
    g_on_close = 0;
    ASSERT_EQ(kl_dgram_close_init(&close, &send, NULL, on_close_cb, NULL), 0);
    KlSockAddr p; kl_sockaddr_from_ipv4(&p, (const unsigned char *)"\x7f\0\0\1", 53);
    KlDatagramMessage m = { .data = "x", .len = 1, .peer = &p, .tos = -1 };
    ASSERT_EQ((int)kl_dgram_send(&send, &m), (int)KL_DATAGRAM_ACCEPTED);   /* queued; submit failed → sticky err */
    ASSERT_TRUE(kl_dgram_send_error(&send) != 0);
    ASSERT_TRUE(kl_dgram_send_queued(&send) > 0);               /* datagram retained */
    ASSERT_EQ(kl_dgram_close_begin(&close), 0);                 /* graceful → escalate (error) → discard */
    ASSERT_EQ((int)kl_dgram_send_queued(&send), 0);
    ASSERT_EQ(kl_dgram_close_is_detached(&close), 1);
    ASSERT_EQ(g_on_close, 1);
    kl_dgram_close_free(&close);
    kl_dgram_send_free(&send);
    kl_dgram_slots_free(&slots);
}

/* a cancel hook may re-enter close/cancel; detachment is deferred to the outermost frame (once) */
UTEST(dgram_close, cancel_reentrant_from_hook) {
    Fix f; fix_init(&f, KL_DGRAM_CAP_CONNECTED);
    ASSERT_EQ(kl_dgram_close_set_cancel(&f.close, cancel_recv, cancel_send, NULL), 0);
    send_one(&f);
    ASSERT_EQ(kl_dgram_recv_start(&f.recv), 0);
    g_cancel_mode = CANCEL_REENTER;                        /* cancel hook calls kl_dgram_close_cancel */
    ASSERT_EQ(kl_dgram_close_cancel(&f.close), 0);
    ASSERT_EQ(g_cancel_send_calls, 1);                     /* still cancel-once despite reentry */
    ASSERT_EQ(g_cancel_recv_calls, 1);
    /* ops async — retire them; exactly one detachment */
    kl_dgram_send_on_complete(&f.send, 0);
    kl_dgram_recv_on_complete(&f.recv, 0, 0);
    ASSERT_EQ(g_on_close, 1);
    fix_free(&f);
}

/* free is refused before confirmed detachment */
UTEST(dgram_close, free_refused_before_detachment) {
    Fix f; fix_init(&f, KL_DGRAM_CAP_CONNECTED);
    send_one(&f);
    ASSERT_EQ(kl_dgram_recv_start(&f.recv), 0);
    ASSERT_EQ(kl_dgram_close_begin(&f.close), 0);
    ASSERT_EQ(kl_dgram_close_free(&f.close), -1);          /* not detached — refused */
    kl_dgram_send_on_complete(&f.send, 1);
    kl_dgram_recv_on_complete(&f.recv, 0, 0);
    ASSERT_EQ(kl_dgram_close_is_detached(&f.close), 1);
    fix_free(&f);                                          /* close_free succeeds now */
}

/* on_close is a destructive tail: the callback tears everything down with no coordinator self-access */
static Fix *g_de_fix;
static int  g_de_fired;
static void destructive_close(void *ctx, KlDatagramCloseResult result) { (void)result;
    (void)ctx;
    g_de_fired = 1;
    /* Everything is retired at detachment → safe to free the whole fixture inside on_close. */
    kl_dgram_close_free(&g_de_fix->close);
    kl_dgram_recv_free(&g_de_fix->recv);
    kl_dgram_send_free(&g_de_fix->send);
    kl_dgram_slots_free(&g_de_fix->slots);
    kl_dgram_inbound_free(&g_de_fix->inbound);
}
UTEST(dgram_close, destructive_on_close_tail) {
    static Fix f;   /* static so the destructive callback frees it by pointer */
    g_a = kl_allocator_default();
    ASSERT_EQ(kl_dgram_slots_init(&f.slots, &g_a, 4, 64), 0);
    ASSERT_EQ(kl_dgram_inbound_init(&f.inbound, &g_a, 64), 0);
    ASSERT_EQ(kl_dgram_send_init(&f.send, &f.slots, &g_a, 1, KL_DGRAM_CAP_CONNECTED, 0, send_submit, NULL), 0);
    ASSERT_EQ(kl_dgram_recv_init(&f.recv, &f.inbound, 1, recv_deliver, NULL, recv_arm, NULL, NULL, NULL), 0);
    ASSERT_EQ(kl_dgram_close_init(&f.close, &f.send, &f.recv, destructive_close, NULL), 0);
    g_de_fix = &f; g_de_fired = 0;
    send_one(&f);
    ASSERT_EQ(kl_dgram_recv_start(&f.recv), 0);
    ASSERT_EQ(kl_dgram_close_begin(&f.close), 0);
    kl_dgram_send_on_complete(&f.send, 1);
    kl_dgram_recv_on_complete(&f.recv, 0, 0);   /* last retirement → detach → destructive on_close */
    ASSERT_EQ(g_de_fired, 1);
    /* everything is freed; do not touch f. */
}

/* ── callback-initiated close: detachment must happen ONCE and only after the originating machine
 *    frame has unwound. These run under ASan — a mid-frame detach+free would be a UAF. ───────────── */

/* close from a RECEIVE DELIVERY callback (recv-only coordinator). */
UTEST(dgram_close, close_from_recv_delivery) {
    KlDgramInbound inbound; KlAllocator a = kl_allocator_default();
    ASSERT_EQ(kl_dgram_inbound_init(&inbound, &a, 64), 0);
    KlDgramRecv recv;
    ASSERT_EQ(kl_dgram_recv_init(&recv, &inbound, 1, closing_deliver, NULL,
                                 recv_arm, NULL, NULL, NULL), 0);
    KlDgramClose close;
    ASSERT_EQ(kl_dgram_close_init(&close, NULL, &recv, on_close_cb, NULL), 0);
    g_cb_close = &close; g_cb_did = 0; g_on_close = 0;
    ASSERT_EQ(kl_dgram_recv_start(&recv), 0);              /* posts; recv in flight */
    KlDgramSlot *in = kl_dgram_inbound_slot(&inbound);
    kl_sockaddr_from_ipv4(&in->peer, (const unsigned char *)"\x7f\0\0\1", 53);   /* mandatory peer */
    memcpy(in->data, "PKT!", 4);
    ASSERT_EQ(kl_dgram_recv_on_complete(&recv, 4, 1), 0);  /* deliver → close_begin (deferred) → detach */
    ASSERT_EQ(g_on_close, 1);                              /* exactly once */
    ASSERT_EQ(kl_dgram_close_is_detached(&close), 1);      /* only after on_complete's frame unwinds */
    ASSERT_EQ(kl_dgram_close_free(&close), 0);
    ASSERT_EQ(kl_dgram_recv_free(&recv), 0);
    kl_dgram_inbound_free(&inbound);
}

/* close from an ON_WRITABLE callback (send-only, single slot → A fills → full→non-full fires). */
UTEST(dgram_close, close_from_on_writable) {
    KlDgramSlots slots; KlAllocator a = kl_allocator_default();
    ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 1, 64), 0);   /* one slot → A makes it full */
    KlDgramSend send;
    ASSERT_EQ(kl_dgram_send_init(&send, &slots, &a, 1, KL_DGRAM_CAP_CONNECTED, 0, send_submit, NULL), 0);
    kl_dgram_send_set_writable_cb(&send, closing_writable, NULL);
    KlDgramClose close;
    ASSERT_EQ(kl_dgram_close_init(&close, &send, NULL, on_close_cb, NULL), 0);
    g_cb_close = &close; g_cb_did = 0; g_on_close = 0;
    KlSockAddr p; kl_sockaddr_from_ipv4(&p, (const unsigned char *)"\x7f\0\0\1", 53);
    KlDatagramMessage m = { .data = "x", .len = 1, .peer = &p, .tos = -1 };
    ASSERT_EQ((int)kl_dgram_send(&send, &m), (int)KL_DATAGRAM_ACCEPTED);  /* A in flight, slots full */
    ASSERT_EQ(kl_dgram_send_on_complete(&send, 1), 0);         /* retire → on_writable → close → detach */
    ASSERT_EQ(g_on_close, 1);
    ASSERT_EQ(kl_dgram_close_is_detached(&close), 1);
    ASSERT_EQ(kl_dgram_close_free(&close), 0);
    ASSERT_EQ(kl_dgram_send_free(&send), 0);
    kl_dgram_slots_free(&slots);
}

/* close from an ON_DRAIN callback (send-only, 4 slots → A never fills → only the drain edge fires). */
UTEST(dgram_close, close_from_on_drain) {
    KlDgramSlots slots; KlAllocator a = kl_allocator_default();
    ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64), 0);
    KlDgramSend send;
    ASSERT_EQ(kl_dgram_send_init(&send, &slots, &a, 1, KL_DGRAM_CAP_CONNECTED, 0, send_submit, NULL), 0);
    kl_dgram_send_set_drain_cb(&send, closing_drain, NULL);
    KlDgramClose close;
    ASSERT_EQ(kl_dgram_close_init(&close, &send, NULL, on_close_cb, NULL), 0);
    g_cb_close = &close; g_cb_did = 0; g_on_close = 0;
    KlSockAddr p; kl_sockaddr_from_ipv4(&p, (const unsigned char *)"\x7f\0\0\1", 53);
    KlDatagramMessage m = { .data = "x", .len = 1, .peer = &p, .tos = -1 };
    ASSERT_EQ((int)kl_dgram_send(&send, &m), (int)KL_DATAGRAM_ACCEPTED);
    ASSERT_EQ(kl_dgram_send_on_complete(&send, 1), 0);         /* retire → queue empty → on_drain → close */
    ASSERT_EQ(g_on_close, 1);
    ASSERT_EQ(kl_dgram_close_is_detached(&close), 1);
    ASSERT_EQ(kl_dgram_close_free(&close), 0);
    ASSERT_EQ(kl_dgram_send_free(&send), 0);
    kl_dgram_slots_free(&slots);
}

/* close from an INLINE ARM completion chain (arm() synchronously completes into a closing delivery). */
UTEST(dgram_close, close_from_inline_arm) {
    KlDgramInbound inbound; KlAllocator a = kl_allocator_default();
    ASSERT_EQ(kl_dgram_inbound_init(&inbound, &a, 64), 0);
    KlDgramRecv recv;
    ASSERT_EQ(kl_dgram_recv_init(&recv, &inbound, 1, closing_deliver, NULL,
                                 inline_close_arm, NULL, NULL, NULL), 0);
    KlDgramClose close;
    ASSERT_EQ(kl_dgram_close_init(&close, NULL, &recv, on_close_cb, NULL), 0);
    g_ia_recv = &recv; g_ia_slots = &inbound; g_cb_close = &close; g_cb_did = 0; g_on_close = 0;
    ASSERT_EQ(kl_dgram_recv_start(&recv), 0);   /* arm→inline complete→deliver→close→detach at unwind */
    ASSERT_EQ(g_on_close, 1);
    ASSERT_EQ(kl_dgram_close_is_detached(&close), 1);
    ASSERT_EQ(kl_dgram_close_free(&close), 0);
    ASSERT_EQ(kl_dgram_recv_free(&recv), 0);
    kl_dgram_inbound_free(&inbound);
}

/* ── reentrant close from the readiness DIRECT-SEND submit hook (busy-frame coverage of the fast
 *    path). Detachment must be deferred to kl_dgram_send's final leave, never mid-hook (ASan). ──── */

/* DONE: the datagram was sent; nothing queued; close finalizes once send returns. */
UTEST(dgram_close, reentrant_close_readiness_done) {
    int st, q, d, oc;
    run_reentrant_close(KL_DGRAM_SUBMIT_DONE, &st, &q, &d, &oc);
    ASSERT_EQ(st, (int)KL_DATAGRAM_ACCEPTED);
    ASSERT_EQ(q, 0);
    ASSERT_EQ(d, 1);                          /* detached exactly once, after the frame unwound */
    ASSERT_EQ(oc, 1);
}

/* WOULD_BLOCK after a reentrant close: MUST NOT enqueue (closing began) — returns CLOSED, queue empty. */
UTEST(dgram_close, reentrant_close_readiness_wouldblock) {
    int st, q, d, oc;
    run_reentrant_close(KL_DGRAM_SUBMIT_WOULDBLOCK, &st, &q, &d, &oc);
    ASSERT_EQ(st, (int)KL_DATAGRAM_CLOSED);   /* re-checked closing state; refused enqueue */
    ASSERT_EQ(q, 0);                          /* no slot taken after closing began */
    ASSERT_EQ(d, 1);
    ASSERT_EQ(oc, 1);
}

/* ERROR: sticky error set on the still-valid object (no UAF); returns ERROR; close finalizes. */
UTEST(dgram_close, reentrant_close_readiness_error) {
    int st, q, d, oc;
    run_reentrant_close(KL_DGRAM_SUBMIT_ERROR, &st, &q, &d, &oc);
    ASSERT_EQ(st, (int)KL_DATAGRAM_ERROR);
    ASSERT_EQ(q, 0);
    ASSERT_EQ(d, 1);
    ASSERT_EQ(oc, 1);
}

/* ══ §4.3 retirement classification — the terminal close RESULT via the backend classifier ═══════ */

static KlDgramRetireResult g_rt_recv, g_rt_send;
static int g_rt_te_recv, g_rt_te_send;
static KlDgramRetireResult test_retire(void *ctx, KlDgramOpKind kind, int *te) {
    (void)ctx;
    if (kind == KL_DGRAM_OP_RECV) { *te = g_rt_te_recv; return g_rt_recv; }
    *te = g_rt_te_send; return g_rt_send;
}
/* Drive a send+recv fixture (classifier installed) to full machine retirement. The classifier then
 * resolves the terminal result. Caller sets g_rt_* first; f is left CLOSED (fix_free-able). */
static void drive_classified(Fix *f) {
    fix_init(f, KL_DGRAM_CAP_CONNECTED);
    kl_dgram_close_set_retire(&f->close, test_retire, NULL);
    send_one(f);
    kl_dgram_recv_start(&f->recv);
    kl_dgram_close_begin(&f->close);
    kl_dgram_send_on_complete(&f->send, 1);   /* send op terminal (inflight → 0) */
    kl_dgram_recv_on_complete(&f->recv, 0, 0);/* recv op terminal → classifier resolves the result */
}

/* No classifier → every op is confirmed RETIRED → DETACHED (the legacy/readiness model). */
UTEST(dgram_close_result, no_classifier_is_detached) {
    Fix f; fix_init(&f, KL_DGRAM_CAP_CONNECTED); g_on_close = 0;
    send_one(&f); kl_dgram_recv_start(&f.recv); kl_dgram_close_begin(&f.close);
    kl_dgram_send_on_complete(&f.send, 1); kl_dgram_recv_on_complete(&f.recv, 0, 0);
    ASSERT_EQ(g_on_close, 1);
    ASSERT_EQ((int)g_on_close_result, (int)KL_DGRAM_DETACHED);
    ASSERT_EQ((int)kl_dgram_close_result(&f.close), (int)KL_DGRAM_DETACHED);
    fix_free(&f);
}

/* All ops RETIRED (classifier) → DETACHED. */
UTEST(dgram_close_result, all_retired_is_detached) {
    g_rt_recv = g_rt_send = KL_DGRAM_RETIRE_RETIRED; g_rt_te_recv = g_rt_te_send = 0; g_on_close = 0;
    Fix f; drive_classified(&f);
    ASSERT_EQ(g_on_close, 1);
    ASSERT_EQ((int)g_on_close_result, (int)KL_DGRAM_DETACHED);
    fix_free(&f);
}

/* Any op QUARANTINED → QUARANTINED (not confirmed detachment). */
UTEST(dgram_close_result, one_quarantined_is_quarantined) {
    g_rt_recv = KL_DGRAM_RETIRE_QUARANTINED; g_rt_send = KL_DGRAM_RETIRE_RETIRED;
    g_rt_te_recv = g_rt_te_send = 0; g_on_close = 0;
    Fix f; drive_classified(&f);
    ASSERT_EQ(g_on_close, 1);
    ASSERT_EQ((int)g_on_close_result, (int)KL_DGRAM_QUARANTINED);
    ASSERT_EQ((int)kl_dgram_close_result(&f.close), (int)KL_DGRAM_QUARANTINED);
    fix_free(&f);
}

/* All RETIRED + a transport error → CLOSE_ERROR. */
UTEST(dgram_close_result, retired_with_transport_error_is_close_error) {
    g_rt_recv = g_rt_send = KL_DGRAM_RETIRE_RETIRED; g_rt_te_recv = 0; g_rt_te_send = 1; g_on_close = 0;
    Fix f; drive_classified(&f);
    ASSERT_EQ(g_on_close, 1);
    ASSERT_EQ((int)g_on_close_result, (int)KL_DGRAM_CLOSE_ERROR);
    fix_free(&f);
}

/* The safety rule: a transport error alongside a QUARANTINED op is QUARANTINED, NEVER CLOSE_ERROR
 * (CLOSE_ERROR is legal only when every op is confirmed RETIRED). */
UTEST(dgram_close_result, quarantine_dominates_transport_error) {
    g_rt_recv = KL_DGRAM_RETIRE_QUARANTINED; g_rt_send = KL_DGRAM_RETIRE_RETIRED;
    g_rt_te_recv = 0; g_rt_te_send = 1; g_on_close = 0;
    Fix f; drive_classified(&f);
    ASSERT_EQ(g_on_close, 1);
    ASSERT_EQ((int)g_on_close_result, (int)KL_DGRAM_QUARANTINED);
    fix_free(&f);
}

/* A PENDING classification keeps the object CLOSING (no on_close, result NONE); a later re-run once
 * the op is RETIRED detaches. */
UTEST(dgram_close_result, pending_stays_closing_then_detaches) {
    g_rt_recv = KL_DGRAM_RETIRE_PENDING; g_rt_send = KL_DGRAM_RETIRE_RETIRED;
    g_rt_te_recv = g_rt_te_send = 0; g_on_close = 0;
    Fix f; drive_classified(&f);
    ASSERT_EQ(g_on_close, 0);                                   /* PENDING → not detached */
    ASSERT_EQ((int)kl_dgram_close_result(&f.close), (int)KL_DGRAM_CLOSE_NONE);
    ASSERT_EQ((int)kl_dgram_close_state(&f.close), (int)KL_DGRAM_CLOSE_CLOSING);

    /* A retry with the op STILL pending is a safe no-op (idempotent). */
    ASSERT_EQ(kl_dgram_close_retry(&f.close), 0);
    ASSERT_EQ(g_on_close, 0);

    g_rt_recv = KL_DGRAM_RETIRE_RETIRED;                        /* op now terminal (resolved out-of-band) */
    ASSERT_EQ(kl_dgram_close_retry(&f.close), 0);              /* the backend-drain progress hook */
    ASSERT_EQ(g_on_close, 1);
    ASSERT_EQ((int)g_on_close_result, (int)KL_DGRAM_DETACHED);
    /* retry after CLOSED is a no-op. */
    ASSERT_EQ(kl_dgram_close_retry(&f.close), 0);
    ASSERT_EQ(g_on_close, 1);
    fix_free(&f);
}

UTEST_MAIN();
