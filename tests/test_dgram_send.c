/*
 * test_dgram_send.c — Phase B step 2: atomic send machinery over KlDgramSlots.
 *
 * Covers: exact status mapping, refusal-takes-no-ownership, copy-before-return, FIFO order over the
 * LIFO free-slot allocator, single-flight enforcement, inline completion (no phantom in-flight),
 * sticky submit failure (never discards), on_writable (full→non-full) / on_drain (non-empty→empty)
 * edges, a reentrant on_writable refill suppressing on_drain, a destructive on_drain tail, teardown
 * refusal while in-flight, and an allocation-free hot path.
 */
#include "utest.h"

#include "../src/datagram_send.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Counting allocator (real malloc/free + counters) — for the allocation-free hot path ─────── */
static int g_mallocs, g_frees;
static void *cnt_malloc(void *c, size_t n)                   { (void)c; g_mallocs++; return malloc(n); }
static void *cnt_realloc(void *c, void *p, size_t o, size_t n){ (void)c; (void)o; return realloc(p, n); }
static void  cnt_free(void *c, void *p, size_t s)           { (void)c; (void)s; g_frees++; free(p); }
static KlAllocator counting_alloc(void) {
    KlAllocator a = { cnt_malloc, cnt_realloc, cnt_free, NULL };
    return a;
}

/* ── Mock submit provider ─────────────────────────────────────────────────────────────────── */
typedef struct {
    KlDgramSubmitResult next;    /* value the next submit returns */
    int    calls;
    unsigned char last[2048];    /* last submitted payload copy */
    size_t last_len;
    int    last_tos;
    int    last_has_peer;
    unsigned char order[64];     /* first byte of each submitted datagram, in submit order */
    int    order_n;
} Mock;
static KlDgramSubmitResult mock_submit(void *ctx, const void *data, size_t len,
                                       const KlSockAddr *peer, const KlSockAddr *local, int tos) {
    (void)local;
    Mock *m = ctx;
    m->calls++;
    if (len <= sizeof(m->last)) { memcpy(m->last, data, len); m->last_len = len; }
    m->last_tos = tos;
    m->last_has_peer = (peer != NULL);
    if (len > 0 && m->order_n < (int)sizeof(m->order))
        m->order[m->order_n++] = ((const unsigned char *)data)[0];
    return m->next;
}

static int g_writable, g_drain;
static void on_writable_cb(void *ctx) { (void)ctx; g_writable++; }
static void on_drain_cb(void *ctx)    { (void)ctx; g_drain++; }

static KlSockAddr any_peer(void) {
    KlSockAddr a; kl_sockaddr_from_ipv4(&a, (const unsigned char *)"\x7f\0\0\1", 9999);
    return a;
}

/* readiness sync provider: direct fast path — no slot churn, no on_drain spam */
UTEST(dgram_send, readiness_sync_direct) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_DONE };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, /*completion*/0, KL_DGRAM_CAP_CONNECTED, mock_submit, &mk), 0);
    kl_dgram_send_set_drain_cb(&s, on_drain_cb, NULL);
    g_drain = 0;
    KlSockAddr p = any_peer();
    for (int i = 0; i < 10; i++) {
        KlDatagramMessage m = { .data = "hello", .len = 5, .peer = &p, .tos = -1 };
        ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);
        ASSERT_EQ((int)kl_dgram_send_queued(&s), 0);          /* direct — nothing queued */
    }
    ASSERT_EQ(mk.calls, 10);
    ASSERT_EQ(g_drain, 0);                                    /* no transient non-empty→empty */
    ASSERT_EQ((int)kl_dgram_slots_free_count(&slots), 4);
    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* readiness WOULD_BLOCK queues (copy), flush drains, on_drain fires once on empty */
UTEST(dgram_send, readiness_wouldblock_then_flush) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_WOULDBLOCK };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 0, KL_DGRAM_CAP_CONNECTED, mock_submit, &mk), 0);
    kl_dgram_send_set_drain_cb(&s, on_drain_cb, NULL);
    g_drain = 0;
    KlSockAddr p = any_peer();
    char buf[5]; memcpy(buf, "AAAAA", 5);
    KlDatagramMessage m = { .data = buf, .len = 5, .peer = &p, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 1);

    memset(buf, 'Z', 5);                                      /* scribble source: slot holds a copy */
    mk.next = KL_DGRAM_SUBMIT_DONE;
    ASSERT_EQ(kl_dgram_send_flush(&s), 0);
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 0);
    ASSERT_EQ((int)mk.last_len, 5);
    ASSERT_EQ(memcmp(mk.last, "AAAAA", 5), 0);
    ASSERT_EQ(g_drain, 1);
    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* completion async: submit (INFLIGHT), retire via on_complete, on_drain on empty */
UTEST(dgram_send, completion_async_retire) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, /*completion*/1, KL_DGRAM_CAP_CONNECTED, mock_submit, &mk), 0);
    kl_dgram_send_set_drain_cb(&s, on_drain_cb, NULL);
    g_drain = 0;
    KlSockAddr p = any_peer();
    KlDatagramMessage m = { .data = "hi", .len = 2, .peer = &p, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);
    ASSERT_EQ((int)kl_dgram_send_inflight(&s), 1);
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 1);
    ASSERT_EQ(mk.calls, 1);

    ASSERT_EQ(kl_dgram_send_on_complete(&s, 1), 0);
    ASSERT_EQ((int)kl_dgram_send_inflight(&s), 0);
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 0);
    ASSERT_EQ(g_drain, 1);
    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* single-flight: a second send queues; only one submit until the first retires (FIFO order) */
UTEST(dgram_send, single_flight_and_fifo_over_lifo) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64, 64), 0);
    /* Scramble the LIFO free-list so acquire hands out non-sequential slots. */
    KlDgramSlot *t[4];
    for (int i = 0; i < 4; i++) t[i] = kl_dgram_slots_acquire(&slots);
    kl_dgram_slots_release(&slots, t[1]);
    kl_dgram_slots_release(&slots, t[3]);
    kl_dgram_slots_release(&slots, t[0]);
    kl_dgram_slots_release(&slots, t[2]);

    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, KL_DGRAM_CAP_CONNECTED, mock_submit, &mk), 0);
    KlSockAddr p = any_peer();
    const char *msgs[4] = { "A...", "B...", "C...", "D..." };
    for (int i = 0; i < 4; i++) {
        KlDatagramMessage m = { .data = msgs[i], .len = 4, .peer = &p, .tos = -1 };
        ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);
    }
    ASSERT_EQ((int)kl_dgram_send_inflight(&s), 1);   /* single-flight: only A submitted */
    ASSERT_EQ(mk.calls, 1);
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 4);
    for (int i = 0; i < 4; i++) ASSERT_EQ(kl_dgram_send_on_complete(&s, 1), 0);
    ASSERT_EQ(mk.order_n, 4);
    ASSERT_EQ(mk.order[0], 'A');   /* FIFO — not the LIFO slot order */
    ASSERT_EQ(mk.order[1], 'B');
    ASSERT_EQ(mk.order[2], 'C');
    ASSERT_EQ(mk.order[3], 'D');
    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* full→non-full fires on_writable once; refusal (WOULD_BLOCK) takes no ownership */
UTEST(dgram_send, writable_edge_and_no_ownership_on_refusal) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 2, 64, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, KL_DGRAM_CAP_CONNECTED, mock_submit, &mk), 0);
    kl_dgram_send_set_writable_cb(&s, on_writable_cb, NULL);
    g_writable = 0;
    KlSockAddr p = any_peer();
    KlDatagramMessage m = { .data = "x", .len = 1, .peer = &p, .tos = -1 };

    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);   /* A submitted (in flight) */
    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);   /* B queued; both slots full */
    ASSERT_EQ((int)kl_dgram_slots_free_count(&slots), 0);
    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_WOULD_BLOCK); /* refused */
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 2);             /* refusal: no ownership taken */
    ASSERT_EQ((int)kl_dgram_slots_free_count(&slots), 0);

    ASSERT_EQ(kl_dgram_send_on_complete(&s, 1), 0);          /* A retires → full→non-full; B submitted */
    ASSERT_EQ(g_writable, 1);
    ASSERT_EQ(kl_dgram_send_on_complete(&s, 1), 0);          /* B retires (queue empties) — not full again */
    ASSERT_EQ(g_writable, 1);
    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* status mapping + none take ownership */
UTEST(dgram_send, status_mapping_and_no_ownership) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 2, 16, 16), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, /*caps*/0, mock_submit, &mk), 0);
    KlSockAddr p = any_peer(), lo = any_peer();

    char big[32]; memset(big, 'B', sizeof(big));
    KlDatagramMessage tl = { .data = big, .len = 32, .peer = &p, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &tl), KL_DATAGRAM_TOO_LARGE);
    KlDatagramMessage sp = { .data = "x", .len = 1, .peer = &p, .local = &lo, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &sp), KL_DATAGRAM_UNSUPPORTED);
    KlDatagramMessage tos = { .data = "x", .len = 1, .peer = &p, .tos = 4 };
    ASSERT_EQ(kl_dgram_send(&s, &tos), KL_DATAGRAM_UNSUPPORTED);
    KlDatagramMessage conn = { .data = "x", .len = 1, .peer = NULL, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &conn), KL_DATAGRAM_UNSUPPORTED);
    KlDatagramMessage bad = { .data = NULL, .len = 4, .peer = &p, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &bad), KL_DATAGRAM_ERROR);
    kl_dgram_send_set_closing(&s, 1);
    KlDatagramMessage ok = { .data = "x", .len = 1, .peer = &p, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &ok), KL_DATAGRAM_CLOSED);

    ASSERT_EQ((int)kl_dgram_send_queued(&s), 0);
    ASSERT_EQ((int)kl_dgram_slots_free_count(&slots), 2);
    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* submission failure is sticky and never discards the datagram */
UTEST(dgram_send, submit_error_is_sticky_and_retains) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_ERROR };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, KL_DGRAM_CAP_CONNECTED, mock_submit, &mk), 0);
    KlSockAddr p = any_peer();
    KlDatagramMessage m = { .data = "keep", .len = 4, .peer = &p, .tos = -1 };

    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);
    ASSERT_TRUE(kl_dgram_send_error(&s) != 0);
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 1);             /* retained — not discarded */
    ASSERT_EQ((int)kl_dgram_send_inflight(&s), 0);
    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ERROR);     /* sticky */
    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* inline completion (submit hook calls on_complete synchronously) → no phantom in-flight */
static KlDgramSend *g_inline_s;
static KlDgramSubmitResult inline_submit(void *ctx, const void *data, size_t len,
                                         const KlSockAddr *peer, const KlSockAddr *local, int tos) {
    Mock *m = ctx; (void)peer; (void)local; (void)tos;
    m->calls++;
    if (len > 0 && m->order_n < (int)sizeof(m->order)) m->order[m->order_n++] = ((const unsigned char *)data)[0];
    kl_dgram_send_on_complete(g_inline_s, 1);   /* retire inline, before returning INFLIGHT */
    return KL_DGRAM_SUBMIT_INFLIGHT;
}
UTEST(dgram_send, inline_completion_no_phantom) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, KL_DGRAM_CAP_CONNECTED, inline_submit, &mk), 0);
    g_inline_s = &s;
    kl_dgram_send_set_drain_cb(&s, on_drain_cb, NULL);
    g_drain = 0;
    KlSockAddr p = any_peer();
    const char *msgs[3] = { "A", "B", "C" };
    for (int i = 0; i < 3; i++) {
        KlDatagramMessage m = { .data = msgs[i], .len = 1, .peer = &p, .tos = -1 };
        ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);
        ASSERT_EQ((int)kl_dgram_send_inflight(&s), 0);       /* retired inline — no phantom stuck at 1 */
        ASSERT_EQ((int)kl_dgram_send_queued(&s), 0);
    }
    ASSERT_EQ(mk.calls, 3);
    ASSERT_EQ(mk.order[0], 'A'); ASSERT_EQ(mk.order[1], 'B'); ASSERT_EQ(mk.order[2], 'C');
    ASSERT_EQ(g_drain, 3);                                    /* each send drained to empty */
    ASSERT_EQ(kl_dgram_send_free(&s), 0);                     /* not stuck in-flight */
    kl_dgram_slots_free(&slots);
}

/* a reentrant on_writable send refills the queue → on_drain is suppressed (still non-empty) */
static KlDgramSend *g_re_s;
static int          g_re_sent, g_re_status;
static size_t       g_re_free_seen;
static void refill_writable(void *ctx) {
    (void)ctx;
    g_writable++;
    if (g_re_sent) return;
    g_re_sent = 1;
    g_re_free_seen = kl_dgram_slots_free_count(g_re_s->slots);   /* observe fully-updated state */
    KlSockAddr p = any_peer();
    KlDatagramMessage m = { .data = "R", .len = 1, .peer = &p, .tos = -1 };
    g_re_status = (int)kl_dgram_send(g_re_s, &m);                 /* refill into the freed slot */
}
UTEST(dgram_send, writable_refill_suppresses_drain) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 1, 32, 32), 0);   /* single slot */
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, KL_DGRAM_CAP_CONNECTED, mock_submit, &mk), 0);
    g_re_s = &s; g_re_sent = 0; g_re_status = -1; g_re_free_seen = 99;
    kl_dgram_send_set_writable_cb(&s, refill_writable, NULL);
    kl_dgram_send_set_drain_cb(&s, on_drain_cb, NULL);
    g_writable = 0; g_drain = 0;
    KlSockAddr p = any_peer();
    KlDatagramMessage m = { .data = "x", .len = 1, .peer = &p, .tos = -1 };

    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);   /* A in flight; the 1 slot is full */
    ASSERT_EQ((int)kl_dgram_slots_free_count(&slots), 0);

    /* A retires → full→non-full → on_writable → reentrant send B refills the single slot. */
    ASSERT_EQ(kl_dgram_send_on_complete(&s, 1), 0);
    ASSERT_EQ(g_re_sent, 1);
    ASSERT_EQ((int)g_re_free_seen, 1);                        /* callback saw the freed slot */
    ASSERT_EQ(g_re_status, KL_DATAGRAM_ACCEPTED);            /* reentrant send succeeded */
    ASSERT_EQ(g_writable, 1);
    ASSERT_EQ(g_drain, 0);                                    /* refilled → on_drain suppressed */
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 1);             /* B occupies the slot (in flight) */

    ASSERT_EQ(kl_dgram_send_on_complete(&s, 1), 0);          /* B retires, no refill → drain now */
    ASSERT_EQ(g_drain, 1);
    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* on_drain is a destructive tail: the callback may free the machine + slots without a UAF */
static KlDgramSend  *g_de_s;
static KlDgramSlots *g_de_slots;
static int           g_de_fired, g_de_free_rc;
static void destructive_drain(void *ctx) {
    (void)ctx;
    g_de_fired = 1;
    /* Nothing in flight at on_drain time (queue empty) → free is allowed. Tear everything down. */
    g_de_free_rc = kl_dgram_send_free(g_de_s);
    kl_dgram_slots_free(g_de_slots);
}
UTEST(dgram_send, destructive_on_drain_tail) {
    KlAllocator a = kl_allocator_default();
    static KlDgramSlots slots;   /* static so the destructive callback can free them by pointer */
    static KlDgramSend  s;
    ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 2, 32, 32), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, KL_DGRAM_CAP_CONNECTED, mock_submit, &mk), 0);
    g_de_s = &s; g_de_slots = &slots; g_de_fired = 0;
    kl_dgram_send_set_drain_cb(&s, destructive_drain, NULL);
    KlSockAddr p = any_peer();
    KlDatagramMessage m = { .data = "x", .len = 1, .peer = &p, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);

    /* Completing the last in-flight send empties the queue → on_drain frees s + slots. The machine
     * must make no access to itself after firing on_drain (ASan/UBSan would catch a UAF). */
    ASSERT_EQ(kl_dgram_send_on_complete(&s, 1), 0);
    ASSERT_EQ(g_de_fired, 1);
    ASSERT_EQ(g_de_free_rc, 0);   /* the destructive free succeeded */
    /* s + slots are freed; do not touch them. */
}

/* teardown refusal while a provider-referenced send is outstanding */
UTEST(dgram_send, free_refused_while_inflight) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 2, 32, 32), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, KL_DGRAM_CAP_CONNECTED, mock_submit, &mk), 0);
    KlSockAddr p = any_peer();
    KlDatagramMessage m = { .data = "x", .len = 1, .peer = &p, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);
    ASSERT_EQ((int)kl_dgram_send_inflight(&s), 1);
    ASSERT_EQ(kl_dgram_send_free(&s), -1);                   /* refused */
    ASSERT_EQ(kl_dgram_send_on_complete(&s, 1), 0);
    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* the send/complete/flush hot path performs no allocation */
UTEST(dgram_send, hot_path_is_allocation_free) {
    KlAllocator a = counting_alloc();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, KL_DGRAM_CAP_CONNECTED, mock_submit, &mk), 0);
    int base = g_mallocs;
    KlSockAddr p = any_peer();
    for (int round = 0; round < 100; round++) {
        KlDatagramMessage m = { .data = "payload", .len = 7, .peer = &p, .tos = -1 };
        (void)kl_dgram_send(&s, &m);
        kl_dgram_send_on_complete(&s, 1);
    }
    ASSERT_EQ(g_mallocs, base);    /* completion send/complete hot path: no allocation */

    Mock mk2 = { .next = KL_DGRAM_SUBMIT_WOULDBLOCK };
    KlDgramSend r;
    ASSERT_EQ(kl_dgram_send_init(&r, &slots, &a, 0, KL_DGRAM_CAP_CONNECTED, mock_submit, &mk2), 0);
    int base2 = g_mallocs;
    for (int i = 0; i < 4; i++) {
        KlDatagramMessage m = { .data = "q", .len = 1, .peer = &p, .tos = -1 };
        (void)kl_dgram_send(&r, &m);
    }
    mk2.next = KL_DGRAM_SUBMIT_DONE;
    kl_dgram_send_flush(&r);
    ASSERT_EQ(g_mallocs, base2);   /* readiness queue+flush hot path: no allocation */

    ASSERT_EQ(kl_dgram_send_free(&r), 0);
    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* teardown symmetry: freeing a machine that still holds queued-but-unsubmitted (WOULD_BLOCK)
 * datagrams releases their slots back to the borrowed pool, so a fresh machine sees full capacity. */
UTEST(dgram_send, readiness_wouldblock_free_restores_slots) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64, 64), 0);
    int free0 = (int)kl_dgram_slots_free_count(&slots);
    Mock mk = { .next = KL_DGRAM_SUBMIT_WOULDBLOCK };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 0, KL_DGRAM_CAP_CONNECTED, mock_submit, &mk), 0);
    KlSockAddr p = any_peer();
    KlDatagramMessage m = { .data = "xy", .len = 2, .peer = &p, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);       /* direct WB → enqueue */
    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);       /* pump WB → stays queued */
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 2);
    ASSERT_EQ((int)kl_dgram_slots_free_count(&slots), free0 - 2); /* two slots consumed */

    ASSERT_EQ(kl_dgram_send_free(&s), 0);                         /* free WITHOUT flushing/discarding */
    ASSERT_EQ((int)kl_dgram_slots_free_count(&slots), free0);     /* all outbound slots restored */

    /* Prove the pool is fully reusable: a fresh machine can queue every slot again. */
    Mock mk2 = { .next = KL_DGRAM_SUBMIT_WOULDBLOCK };
    KlDgramSend s2;
    ASSERT_EQ(kl_dgram_send_init(&s2, &slots, &a, 0, KL_DGRAM_CAP_CONNECTED, mock_submit, &mk2), 0);
    for (int i = 0; i < free0; i++)
        ASSERT_EQ(kl_dgram_send(&s2, &m), KL_DATAGRAM_ACCEPTED);
    ASSERT_EQ((int)kl_dgram_send_queued(&s2), free0);
    ASSERT_EQ((int)kl_dgram_slots_free_count(&slots), 0);         /* full capacity was available */
    ASSERT_EQ(kl_dgram_send_free(&s2), 0);                        /* releases the queued slots again */
    ASSERT_EQ((int)kl_dgram_slots_free_count(&slots), free0);
    kl_dgram_slots_free(&slots);
}

UTEST_MAIN();
