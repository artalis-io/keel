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
static void on_close_cb(void *ctx) { (void)ctx; g_on_close++; }

/* Fixtures: slots + a completion send + a completion recv, wired into a close coordinator. */
typedef struct { KlDgramSlots slots; KlDgramSend send; KlDgramRecv recv; KlDgramClose close; } Fix;
static KlAllocator g_a;
static void fix_init(Fix *f, unsigned caps) {   /* fixed valid args → each init returns 0 */
    g_a = kl_allocator_default();
    kl_dgram_slots_init(&f->slots, &g_a, 4, 64, 64);
    kl_dgram_send_init(&f->send, &f->slots, &g_a, /*completion*/1, caps, send_submit, NULL);
    kl_dgram_recv_init(&f->recv, &f->slots, /*completion*/1, recv_deliver, NULL,
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

/* a send error during graceful drain must not leave the close permanently waiting on the queue */
UTEST(dgram_close, error_during_graceful_drain) {
    Fix f; fix_init(&f, KL_DGRAM_CAP_CONNECTED);
    send_one(&f);                                          /* A in flight */
    send_one(&f);                                          /* B queued (cannot drain after the error) */
    ASSERT_EQ(kl_dgram_close_begin(&f.close), 0);
    ASSERT_EQ(kl_dgram_close_is_detached(&f.close), 0);
    ASSERT_EQ(kl_dgram_send_on_complete(&f.send, 0), -1);  /* A fails → sticky error; B undrainable */
    ASSERT_TRUE(kl_dgram_send_error(&f.send) != 0);
    ASSERT_TRUE(kl_dgram_send_queued(&f.send) > 0);        /* queue not empty ... */
    ASSERT_EQ(kl_dgram_close_is_detached(&f.close), 1);    /* ... but error prevents waiting → detached */
    ASSERT_EQ(g_on_close, 1);
    fix_free(&f);
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
static void destructive_close(void *ctx) {
    (void)ctx;
    g_de_fired = 1;
    /* Everything is retired at detachment → safe to free the whole fixture inside on_close. */
    kl_dgram_close_free(&g_de_fix->close);
    kl_dgram_recv_free(&g_de_fix->recv);
    kl_dgram_send_free(&g_de_fix->send);
    kl_dgram_slots_free(&g_de_fix->slots);
}
UTEST(dgram_close, destructive_on_close_tail) {
    static Fix f;   /* static so the destructive callback frees it by pointer */
    g_a = kl_allocator_default();
    ASSERT_EQ(kl_dgram_slots_init(&f.slots, &g_a, 4, 64, 64), 0);
    ASSERT_EQ(kl_dgram_send_init(&f.send, &f.slots, &g_a, 1, KL_DGRAM_CAP_CONNECTED, send_submit, NULL), 0);
    ASSERT_EQ(kl_dgram_recv_init(&f.recv, &f.slots, 1, recv_deliver, NULL, recv_arm, NULL, NULL, NULL), 0);
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

UTEST_MAIN();
