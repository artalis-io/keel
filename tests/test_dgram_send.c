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
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_DONE };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, /*completion*/0, KL_DGRAM_CAP_CONNECTED, 0, mock_submit, &mk), 0);
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
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_WOULDBLOCK };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 0, KL_DGRAM_CAP_CONNECTED, 0, mock_submit, &mk), 0);
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
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, /*completion*/1, KL_DGRAM_CAP_CONNECTED, 0, mock_submit, &mk), 0);
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
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64), 0);
    /* Scramble the LIFO free-list so acquire hands out non-sequential slots. */
    KlDgramSlot *t[4];
    for (int i = 0; i < 4; i++) t[i] = kl_dgram_slots_acquire(&slots);
    kl_dgram_slots_release(&slots, t[1]);
    kl_dgram_slots_release(&slots, t[3]);
    kl_dgram_slots_release(&slots, t[0]);
    kl_dgram_slots_release(&slots, t[2]);

    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, KL_DGRAM_CAP_CONNECTED, 0, mock_submit, &mk), 0);
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
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 2, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, KL_DGRAM_CAP_CONNECTED, 0, mock_submit, &mk), 0);
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
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 2, 16), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, /*caps*/0, 0, mock_submit, &mk), 0);
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
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_ERROR };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, KL_DGRAM_CAP_CONNECTED, 0, mock_submit, &mk), 0);
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
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, KL_DGRAM_CAP_CONNECTED, 0, inline_submit, &mk), 0);
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
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 1, 32), 0);   /* single slot */
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, KL_DGRAM_CAP_CONNECTED, 0, mock_submit, &mk), 0);
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
    ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 2, 32), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, KL_DGRAM_CAP_CONNECTED, 0, mock_submit, &mk), 0);
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
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 2, 32), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, KL_DGRAM_CAP_CONNECTED, 0, mock_submit, &mk), 0);
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
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, KL_DGRAM_CAP_CONNECTED, 0, mock_submit, &mk), 0);
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
    ASSERT_EQ(kl_dgram_send_init(&r, &slots, &a, 0, KL_DGRAM_CAP_CONNECTED, 0, mock_submit, &mk2), 0);
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
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64), 0);
    int free0 = (int)kl_dgram_slots_free_count(&slots);
    Mock mk = { .next = KL_DGRAM_SUBMIT_WOULDBLOCK };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 0, KL_DGRAM_CAP_CONNECTED, 0, mock_submit, &mk), 0);
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
    ASSERT_EQ(kl_dgram_send_init(&s2, &slots, &a, 0, KL_DGRAM_CAP_CONNECTED, 0, mock_submit, &mk2), 0);
    for (int i = 0; i < free0; i++)
        ASSERT_EQ(kl_dgram_send(&s2, &m), KL_DATAGRAM_ACCEPTED);
    ASSERT_EQ((int)kl_dgram_send_queued(&s2), free0);
    ASSERT_EQ((int)kl_dgram_slots_free_count(&slots), 0);         /* full capacity was available */
    ASSERT_EQ(kl_dgram_send_free(&s2), 0);                        /* releases the queued slots again */
    ASSERT_EQ((int)kl_dgram_slots_free_count(&slots), free0);
    kl_dgram_slots_free(&slots);
}

/* ══ M1 — BOTH byte-gate send-queue policy (docs/datagram_m1_queue_policy_design.md §10) ══════════
 * SLOT (byte_budget 0) is exercised by every test above (§10.1 regression). These cover BOTH. */

/* §10.2 — BOTH byte-gate refusal (case b): the byte budget refuses while a slot is still free, and
 * because bytes_used > 0 (count ≥ 1) a retirement re-signals writable. */
UTEST(dgram_send, both_byte_gate_refuses_with_free_slot) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 8, 64), 0);  /* plenty of slots */
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, /*completion*/1, KL_DGRAM_CAP_CONNECTED,
                                 /*byte_budget*/20, mock_submit, &mk), 0);
    kl_dgram_send_set_writable_cb(&s, on_writable_cb, NULL);
    g_writable = 0;
    KlSockAddr p = any_peer();
    KlDatagramMessage m10 = { .data = "0123456789", .len = 10, .peer = &p, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &m10), KL_DATAGRAM_ACCEPTED);   /* bytes_used 10 (in flight) */
    ASSERT_EQ(kl_dgram_send(&s, &m10), KL_DATAGRAM_ACCEPTED);   /* bytes_used 20 (queued) */
    ASSERT_EQ((int)s.bytes_used, 20);
    ASSERT_EQ(kl_dgram_send(&s, &m10), KL_DATAGRAM_WOULD_BLOCK); /* case (b): 20+10 > 20 */
    ASSERT_TRUE((int)kl_dgram_slots_free_count(&slots) > 0);     /* refused by BYTES, not slots */
    ASSERT_EQ(s.full, 1);                                        /* full→non-full armed */
    ASSERT_TRUE((int)kl_dgram_send_queued(&s) >= 1);             /* count ≥ 1 → retry will re-signal */
    /* retire the in-flight head → bytes_used drops → on_writable fires */
    ASSERT_EQ(kl_dgram_send_on_complete(&s, 1), 0);
    ASSERT_EQ(g_writable, 1);
    ASSERT_EQ((int)s.bytes_used, 10);                           /* one 10-byte datagram retired */
    kl_dgram_send_abandon(&s);
    kl_dgram_slots_free(&slots);
}

/* §10.3 — under BOTH the SLOT bound still fires first when many small datagrams stay under budget. */
UTEST(dgram_send, both_slot_gate_still_active) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 2, 64), 0);  /* only 2 slots */
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, /*completion*/1, KL_DGRAM_CAP_CONNECTED,
                                 /*byte_budget*/10000, mock_submit, &mk), 0);  /* budget won't bind */
    KlSockAddr p = any_peer();
    KlDatagramMessage m = { .data = "0123456789", .len = 10, .peer = &p, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);
    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);      /* both slots taken */
    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_WOULD_BLOCK);   /* SLOT gate, not byte gate */
    ASSERT_EQ((int)kl_dgram_slots_free_count(&slots), 0);
    ASSERT_TRUE(s.bytes_used < s.byte_budget);                   /* budget had room to spare */
    kl_dgram_send_abandon(&s);
    kl_dgram_slots_free(&slots);
}

/* §10.4 — zero-length datagrams are bounded by SLOTS (not bytes); on_drain keys on count, not
 * bytes_used (which stays 0 throughout) — the P1a regression. */
UTEST(dgram_send, both_zero_length_bounded_by_slots) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 3, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, /*completion*/1, KL_DGRAM_CAP_CONNECTED,
                                 /*byte_budget*/100, mock_submit, &mk), 0);
    kl_dgram_send_set_drain_cb(&s, on_drain_cb, NULL);
    g_drain = 0;
    KlSockAddr p = any_peer();
    KlDatagramMessage z = { .data = NULL, .len = 0, .peer = &p, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &z), KL_DATAGRAM_ACCEPTED);
    ASSERT_EQ(kl_dgram_send(&s, &z), KL_DATAGRAM_ACCEPTED);
    ASSERT_EQ(kl_dgram_send(&s, &z), KL_DATAGRAM_ACCEPTED);      /* 3 zero-length in the 3 slots */
    ASSERT_EQ((int)s.bytes_used, 0);                            /* 0 bytes, yet count == 3 */
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 3);
    ASSERT_EQ(kl_dgram_send(&s, &z), KL_DATAGRAM_WOULD_BLOCK);   /* refused by the SLOT bound */
    /* drain all three via single-flight completions; on_drain fires once on count→0, bytes_used==0 */
    ASSERT_EQ(kl_dgram_send_on_complete(&s, 1), 0);
    ASSERT_EQ(kl_dgram_send_on_complete(&s, 1), 0);
    ASSERT_EQ(kl_dgram_send_on_complete(&s, 1), 0);
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 0);
    ASSERT_EQ((int)s.bytes_used, 0);
    ASSERT_EQ(g_drain, 1);                                      /* drain keyed on count, not bytes */
    kl_dgram_send_abandon(&s);
    kl_dgram_slots_free(&slots);
}

/* §10.5 — case (a) len > byte_budget, readiness: permanent TOO_LARGE on a blocked empty queue (no
 * full, no strand); but a socket-ready direct send still delivers it (KlUdp parity). */
UTEST(dgram_send, both_oversize_readiness_permanent_or_direct) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 128), 0);  /* slot_cap 128 */
    Mock mk = { .next = KL_DGRAM_SUBMIT_WOULDBLOCK };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, /*completion*/0, KL_DGRAM_CAP_CONNECTED,
                                 /*byte_budget*/64, mock_submit, &mk), 0);      /* budget 64 < slot_cap */
    KlSockAddr p = any_peer();
    char big[100]; memset(big, 'X', sizeof(big));
    KlDatagramMessage m = { .data = big, .len = 100, .peer = &p, .tos = -1 };   /* >budget, ≤slot_cap */
    /* socket blocked: fast path WOULD_BLOCK → byte gate case (a) → permanent TOO_LARGE */
    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_TOO_LARGE);
    ASSERT_EQ(s.full, 0);                                       /* NOT armed — nothing to retry */
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 0);               /* nothing queued → no strand */
    /* socket ready: the SAME oversize datagram is delivered directly (budget bypassed) */
    mk.next = KL_DGRAM_SUBMIT_DONE;
    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 0);
    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* §10.6 — case (a), completion: no fast path, so len > byte_budget is refused TOO_LARGE upfront and
 * nothing is submitted (matches KlUdp on a completion loop). */
UTEST(dgram_send, both_oversize_completion_upfront) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 128), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, /*completion*/1, KL_DGRAM_CAP_CONNECTED,
                                 /*byte_budget*/64, mock_submit, &mk), 0);
    KlSockAddr p = any_peer();
    char big[100]; memset(big, 'Y', sizeof(big));
    KlDatagramMessage m = { .data = big, .len = 100, .peer = &p, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_TOO_LARGE);
    ASSERT_EQ(mk.calls, 0);                                     /* never submitted */
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 0);
    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* §10.7 — budget < slot_cap is valid (no budget≥slot_cap requirement): a ≤budget datagram admits, a
 * >budget (but ≤slot_cap) datagram is case (a). */
UTEST(dgram_send, both_budget_below_slot_cap_valid) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 200), 0);  /* slot_cap 200 */
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, /*completion*/1, KL_DGRAM_CAP_CONNECTED,
                                 /*byte_budget*/50, mock_submit, &mk), 0);       /* 50 < 200 */
    KlSockAddr p = any_peer();
    char d40[40]; memset(d40, 'a', sizeof(d40));
    char d90[90]; memset(d90, 'b', sizeof(d90));
    KlDatagramMessage fits = { .data = d40, .len = 40, .peer = &p, .tos = -1 };  /* ≤ budget */
    KlDatagramMessage over = { .data = d90, .len = 90, .peer = &p, .tos = -1 };  /* > budget, ≤ slot_cap */
    ASSERT_EQ(kl_dgram_send(&s, &fits), KL_DATAGRAM_ACCEPTED);
    ASSERT_EQ(kl_dgram_send(&s, &over), KL_DATAGRAM_TOO_LARGE);  /* case (a), not a slot TOO_LARGE */
    kl_dgram_send_abandon(&s);
    kl_dgram_slots_free(&slots);
}

/* §10.8 — retiring a datagram drops bytes_used by exactly its len and reopens admission. */
UTEST(dgram_send, both_accounting_reopens_admission) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, /*completion*/1, KL_DGRAM_CAP_CONNECTED,
                                 /*byte_budget*/20, mock_submit, &mk), 0);
    KlSockAddr p = any_peer();
    KlDatagramMessage m10 = { .data = "0123456789", .len = 10, .peer = &p, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &m10), KL_DATAGRAM_ACCEPTED);   /* bytes_used 10 (in flight) */
    ASSERT_EQ(kl_dgram_send(&s, &m10), KL_DATAGRAM_ACCEPTED);   /* bytes_used 20 (queued) */
    ASSERT_EQ(kl_dgram_send(&s, &m10), KL_DATAGRAM_WOULD_BLOCK); /* budget full (case b) */
    ASSERT_EQ(kl_dgram_send_on_complete(&s, 1), 0);            /* retire head → bytes_used 10 */
    ASSERT_EQ((int)s.bytes_used, 10);
    ASSERT_EQ(kl_dgram_send(&s, &m10), KL_DATAGRAM_ACCEPTED);   /* admission reopened */
    ASSERT_EQ((int)s.bytes_used, 20);
    kl_dgram_send_abandon(&s);
    kl_dgram_slots_free(&slots);
}

/* §10.9 — discard/abandon accounting with in-flight retention (blocker P2). */
UTEST(dgram_send, both_discard_queued_only_readiness) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_WOULDBLOCK };   /* readiness: queues, nothing in flight */
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, /*completion*/0, KL_DGRAM_CAP_CONNECTED,
                                 /*byte_budget*/100, mock_submit, &mk), 0);
    KlSockAddr p = any_peer();
    KlDatagramMessage m10 = { .data = "0123456789", .len = 10, .peer = &p, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &m10), KL_DATAGRAM_ACCEPTED);   /* fast path WOULD_BLOCK → queued */
    ASSERT_EQ(kl_dgram_send(&s, &m10), KL_DATAGRAM_ACCEPTED);   /* queued behind it */
    ASSERT_EQ((int)s.bytes_used, 20);
    ASSERT_EQ((int)kl_dgram_send_inflight(&s), 0);
    kl_dgram_send_discard_queued(&s);
    ASSERT_EQ((int)s.bytes_used, 0);                           /* queued-only → 0 immediately */
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 0);
    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

UTEST(dgram_send, both_discard_retains_inflight_then_completion_zeroes) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, /*completion*/1, KL_DGRAM_CAP_CONNECTED,
                                 /*byte_budget*/100, mock_submit, &mk), 0);
    KlSockAddr p = any_peer();
    KlDatagramMessage m10 = { .data = "0123456789", .len = 10, .peer = &p, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &m10), KL_DATAGRAM_ACCEPTED);   /* in flight (bytes_used 10) */
    ASSERT_EQ(kl_dgram_send(&s, &m10), KL_DATAGRAM_ACCEPTED);   /* queued (bytes_used 20) */
    ASSERT_EQ((int)kl_dgram_send_inflight(&s), 1);
    kl_dgram_send_discard_queued(&s);
    ASSERT_EQ((int)s.bytes_used, 10);                          /* retained in-flight head's len */
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 1);              /* the in-flight slot remains */
    ASSERT_EQ(kl_dgram_send_on_complete(&s, 1), 0);           /* terminal completion */
    ASSERT_EQ((int)s.bytes_used, 0);                          /* now zero */
    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

UTEST(dgram_send, both_abandon_resets_bytes_used) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, /*completion*/1, KL_DGRAM_CAP_CONNECTED,
                                 /*byte_budget*/100, mock_submit, &mk), 0);
    KlSockAddr p = any_peer();
    KlDatagramMessage m10 = { .data = "0123456789", .len = 10, .peer = &p, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &m10), KL_DATAGRAM_ACCEPTED);   /* in flight, bytes_used 10 */
    kl_dgram_send_abandon(&s);                                  /* dead machine → direct reset */
    ASSERT_EQ((int)s.bytes_used, 0);
    kl_dgram_slots_free(&slots);
}

/* §10.9 zero-length interaction: a retained zero-length in-flight head leaves bytes_used == 0 after
 * discard yet count == 1 until completion (P1a/P2 interaction). */
UTEST(dgram_send, both_discard_zero_length_inflight_head) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, /*completion*/1, KL_DGRAM_CAP_CONNECTED,
                                 /*byte_budget*/100, mock_submit, &mk), 0);
    KlSockAddr p = any_peer();
    KlDatagramMessage z    = { .data = NULL, .len = 0,  .peer = &p, .tos = -1 };
    KlDatagramMessage m10  = { .data = "0123456789", .len = 10, .peer = &p, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &z),   KL_DATAGRAM_ACCEPTED);   /* zero-length in flight (0 bytes) */
    ASSERT_EQ(kl_dgram_send(&s, &m10), KL_DATAGRAM_ACCEPTED);   /* 10-byte queued (bytes_used 10) */
    ASSERT_EQ((int)s.bytes_used, 10);
    kl_dgram_send_discard_queued(&s);
    ASSERT_EQ((int)s.bytes_used, 0);                           /* retained head is zero-length */
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 1);              /* yet count == 1 (P1a/P2) */
    ASSERT_EQ(kl_dgram_send_on_complete(&s, 1), 0);
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 0);
    kl_dgram_send_free(&s);
    kl_dgram_slots_free(&slots);
}

/* ══ M5.2a — readiness batch-drain (kl_dgram_send_flush_batch) ═══════════════════════════════════ */

/* submit_batch mock: transmits the leading run of non-'B' descriptors; a 'B' at the head → ERROR. */
static KlDgramBatchResult batch_submit_mark(void *ctx, const KlDgramTxDesc *descs, int n, int *sent) {
    (void)ctx;
    if (n > 0 && ((const char *)descs[0].data)[0] == 'B') { *sent = 0; return KL_DGRAM_BATCH_ERROR; }
    int i = 0;
    while (i < n && ((const char *)descs[i].data)[0] != 'B') i++;
    *sent = i;                       /* the good prefix (stops before a 'B') */
    return KL_DGRAM_BATCH_SENT;
}
/* submit_batch mock: nothing goes out (EAGAIN). */
static KlDgramBatchResult batch_submit_wouldblock(void *ctx, const KlDgramTxDesc *descs, int n, int *sent) {
    (void)ctx; (void)descs; (void)n; *sent = 0; return KL_DGRAM_BATCH_WOULDBLOCK;
}
/* submit_batch mock: transmits at most 2 per call (partial prefix). */
static KlDgramBatchResult batch_submit_half(void *ctx, const KlDgramTxDesc *descs, int n, int *sent) {
    (void)ctx; (void)descs; *sent = (n > 2) ? 2 : n; return KL_DGRAM_BATCH_SENT;
}
/* single-submit for the isolation step: ERROR for a 'B' head, DONE otherwise. */
static KlDgramSubmitResult iso_submit(void *ctx, const void *data, size_t len,
                                      const KlSockAddr *peer, const KlSockAddr *local, int tos) {
    (void)ctx; (void)len; (void)peer; (void)local; (void)tos;
    return (((const char *)data)[0] == 'B') ? KL_DGRAM_SUBMIT_ERROR : KL_DGRAM_SUBMIT_DONE;
}

static void enqueue_marked(KlDgramSend *s, const KlSockAddr *p, const char *marks) {
    for (const char *c = marks; *c; c++) {
        char b = *c;
        KlDatagramMessage m = { .data = &b, .len = 1, .peer = p, .tos = -1 };
        (void)kl_dgram_send_enqueue(s, &m);
    }
}

/* A hard error on an isolated head is a RECOVERABLE drop: the head is dropped (dropped++), the sticky
 * error is NOT set, and the rest of the queue still drains in FIFO order. */
UTEST(dgram_send, flush_batch_recoverable_drop) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 8, 64), 0);
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 0, KL_DGRAM_CAP_CONNECTED, 0, iso_submit, NULL), 0);
    KlSockAddr p = any_peer();
    /* Bad datagram at the head: submit_batch returns ERROR on the head-run → the head is isolated,
     * hard-errors, is dropped, and the drain CONTINUES with the rest. (A middle bad descriptor instead
     * surfaces as a short SENT prefix — transient, drained on the next writable edge per §4.2.) */
    enqueue_marked(&s, &p, "BAC");                        /* B bad (head), A + C good */
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 3);

    KlDgramTxDesc descs[8];
    ASSERT_EQ(kl_dgram_send_flush_batch(&s, descs, 8, batch_submit_mark, NULL), 0);
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 0);          /* fully drained (B dropped, A + C sent) */
    ASSERT_EQ((size_t)1, kl_dgram_send_dropped(&s));      /* B dropped by the recoverable policy */
    ASSERT_EQ(0, kl_dgram_send_error(&s));                /* recoverable — the queue is NOT poisoned */

    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* WOULD_BLOCK on the whole run retains everything (no drop, no sticky error) for a later writable edge. */
UTEST(dgram_send, flush_batch_wouldblock_retains) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 8, 64), 0);
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 0, KL_DGRAM_CAP_CONNECTED, 0, iso_submit, NULL), 0);
    KlSockAddr p = any_peer();
    enqueue_marked(&s, &p, "AAA");
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 3);

    KlDgramTxDesc descs[8];
    ASSERT_EQ(kl_dgram_send_flush_batch(&s, descs, 8, batch_submit_wouldblock, NULL), 0);
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 3);          /* nothing sent — all retained */
    ASSERT_EQ((size_t)0, kl_dgram_send_dropped(&s));
    ASSERT_EQ(0, kl_dgram_send_error(&s));

    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* A batch (recoverable) datagram that hard-errors on the ORDINARY single-flight pump (the path a
 * retained bad-middle slot takes on a later writable edge) is DROPPED, not sticky — the queue keeps
 * draining. (An ordinary single send in the same spot would set the sticky error — unchanged.) */
UTEST(dgram_send, pump_recoverable_drop_of_batch_slot) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 8, 64), 0);
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 0, KL_DGRAM_CAP_CONNECTED, 0, iso_submit, NULL), 0);
    KlSockAddr p = any_peer();
    enqueue_marked(&s, &p, "ABC");                        /* batch-enqueued → recoverable; B hard-errors */
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 3);

    ASSERT_EQ(kl_dgram_send_flush(&s), 0);                /* the single-flight pump drains A, drops B, sends C */
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 0);
    ASSERT_EQ((size_t)1, kl_dgram_send_dropped(&s));      /* B recoverably dropped mid-queue */
    ASSERT_EQ(0, kl_dgram_send_error(&s));                /* NOT poisoned */

    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* An ordinary (single) send that hard-errors stays STICKY — the recoverable policy is batch-only. */
UTEST(dgram_send, single_send_hard_error_is_sticky) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64), 0);
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 0, KL_DGRAM_CAP_CONNECTED, 0, iso_submit, NULL), 0);
    KlSockAddr p = any_peer();
    char b = 'B';
    KlDatagramMessage m = { .data = &b, .len = 1, .peer = &p, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ERROR);  /* fast-path hard error → sticky */
    ASSERT_EQ(1, kl_dgram_send_error(&s));
    ASSERT_EQ((size_t)0, kl_dgram_send_dropped(&s));      /* NOT a recoverable drop */

    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* flush_batch defensively refuses a completion machine or an outstanding in-flight op. */
UTEST(dgram_send, flush_batch_rejects_completion_and_inflight) {
    KlAllocator a = kl_allocator_default();
    KlDgramTxDesc descs[4];

    KlDgramSlots rs; ASSERT_EQ(kl_dgram_slots_init(&rs, &a, 4, 64), 0);
    KlDgramSend rd;
    ASSERT_EQ(kl_dgram_send_init(&rd, &rs, &a, 0, KL_DGRAM_CAP_CONNECTED, 0, iso_submit, NULL), 0);
    rd.inflight_n = 1;                                    /* whitebox: pretend a single-flight op is out */
    ASSERT_EQ(-1, kl_dgram_send_flush_batch(&rd, descs, 4, batch_submit_wouldblock, NULL));
    rd.inflight_n = 0;
    ASSERT_EQ(kl_dgram_send_free(&rd), 0);
    kl_dgram_slots_free(&rs);

    KlDgramSlots cs; ASSERT_EQ(kl_dgram_slots_init(&cs, &a, 4, 64), 0);
    KlDgramSend cd;
    ASSERT_EQ(kl_dgram_send_init(&cd, &cs, &a, /*completion*/1, KL_DGRAM_CAP_CONNECTED, 0, iso_submit, NULL), 0);
    ASSERT_EQ(-1, kl_dgram_send_flush_batch(&cd, descs, 4, batch_submit_wouldblock, NULL));
    ASSERT_EQ(kl_dgram_send_free(&cd), 0);
    kl_dgram_slots_free(&cs);
}

/* A short SENT prefix retires exactly that many and retains the remainder in FIFO order. */
UTEST(dgram_send, flush_batch_partial_prefix) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 8, 64), 0);
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 0, KL_DGRAM_CAP_CONNECTED, 0, iso_submit, NULL), 0);
    KlSockAddr p = any_peer();
    enqueue_marked(&s, &p, "AAAA");
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 4);

    KlDgramTxDesc descs[8];
    ASSERT_EQ(kl_dgram_send_flush_batch(&s, descs, 8, batch_submit_half, NULL), 0);
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 2);          /* 2 sent, 2 retained (short prefix → break) */
    ASSERT_EQ((size_t)0, kl_dgram_send_dropped(&s));

    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* ══ M5.2b — GSO queued group (machine level) ═══════════════════════════════════════════════════ */

/* A whole-group GSO submit hook: DONE unless the payload's first byte marks it — 'W' → WOULDBLOCK,
 * 'U' → UNSUPPORTED, 'E' → ERROR. Records the group submit for ordering assertions. */
static int g_gso_calls, g_gso_done_calls;
static char g_gso_first;
static KlDgramSubmitResult gso_submit(void *ctx, void *owner, const void *buf, size_t total,
                                      size_t seg, const KlSockAddr *peer, int tos) {
    (void)ctx; (void)owner; (void)seg; (void)peer; (void)tos; (void)total;
    g_gso_calls++;
    char c = (buf && total) ? ((const char *)buf)[0] : 0;
    g_gso_first = c;
    if (c == 'W') return KL_DGRAM_SUBMIT_WOULDBLOCK;
    if (c == 'U') return KL_DGRAM_SUBMIT_UNSUPPORTED;
    if (c == 'E') return KL_DGRAM_SUBMIT_ERROR;
    return KL_DGRAM_SUBMIT_DONE;
}
static void gso_done(void *ctx, void *owner) { (void)ctx; (void)owner; g_gso_done_calls++; }

/* A GSO group queued BEHIND an older ordinary datagram is not submitted until that datagram retires —
 * FIFO order (the ordinary 'X' egresses first, then the whole group). */
UTEST(dgram_send, gso_fifo_behind_ordinary) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 8, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_WOULDBLOCK };   /* queue (no fast path) */
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 0, KL_DGRAM_CAP_CONNECTED, 0, mock_submit, &mk), 0);
    kl_dgram_send_set_gso_cbs(&s, gso_submit, NULL, gso_done, NULL);
    g_gso_calls = g_gso_done_calls = 0;
    KlSockAddr p = any_peer();

    char x = 'X';
    KlDatagramMessage m = { .data = &x, .len = 1, .peer = &p, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);            /* ordinary, queued (WOULDBLOCK) */
    static char gbuf[4] = { 'G','G','G','G' };
    ASSERT_EQ(kl_dgram_send_enqueue_gso(&s, gbuf, 4, 2, 2, &p, -1, 0, NULL), KL_DATAGRAM_ACCEPTED);
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 3);                      /* 1 ordinary + 2 GSO segments */

    /* The head is the ordinary datagram — flush submits IT (via the single hook), NOT the group. */
    mk.order_n = 0; mk.next = KL_DGRAM_SUBMIT_WOULDBLOCK;
    ASSERT_EQ(kl_dgram_send_flush(&s), 0);
    ASSERT_EQ(0, g_gso_calls);                                         /* group NOT submitted yet */
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 3);

    /* Let the ordinary datagram go → the group reaches the head → one whole-group send_gso. */
    mk.next = KL_DGRAM_SUBMIT_DONE;
    ASSERT_EQ(kl_dgram_send_flush(&s), 0);
    ASSERT_EQ('X', mk.order[0]);                                       /* ordinary egressed first (FIFO) */
    ASSERT_EQ(1, g_gso_calls);                                         /* then the whole group, once */
    ASSERT_EQ(1, g_gso_done_calls);                                    /* group retired → done fired once */
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 0);

    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* Atomic refusal: not enough free slots, and the BOTH byte budget — nothing is reserved on refusal. */
UTEST(dgram_send, gso_atomic_refusal) {
    KlAllocator a = kl_allocator_default();
    /* slots: only 2 free; a 3-segment group must be refused WHOLE (no partial reservation). */
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 2, 64), 0);
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 0, KL_DGRAM_CAP_CONNECTED, 0, iso_submit, NULL), 0);
    KlSockAddr p = any_peer();
    static char g6[6] = { '1','2','3','4','5','6' };
    ASSERT_EQ(kl_dgram_send_enqueue_gso(&s, g6, 6, 2, 3, &p, -1, 0, NULL), KL_DATAGRAM_TOO_LARGE); /* nseg>slots */
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 0);                      /* nothing reserved */
    ASSERT_EQ((int)kl_dgram_slots_free_count(&slots), 2);
    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);

    /* BOTH byte budget: 4 slots, budget 5 bytes. Hold datagrams queued with a WOULD_BLOCK mock. */
    Mock wb = { .next = KL_DGRAM_SUBMIT_WOULDBLOCK };
    KlDgramSlots s2; ASSERT_EQ(kl_dgram_slots_init(&s2, &a, 4, 64), 0);
    KlDgramSend b;
    ASSERT_EQ(kl_dgram_send_init(&b, &s2, &a, 0, KL_DGRAM_CAP_CONNECTED, /*budget*/5, mock_submit, &wb), 0);
    /* (a) a 6-byte group over the WHOLE budget → permanent TOO_LARGE, nothing reserved. */
    ASSERT_EQ(kl_dgram_send_enqueue_gso(&b, g6, 6, 2, 3, &p, -1, 0, NULL), KL_DATAGRAM_TOO_LARGE);
    ASSERT_EQ((int)kl_dgram_send_queued(&b), 0);
    ASSERT_EQ((int)kl_dgram_slots_free_count(&s2), 4);
    /* (b) fits the budget but not right now: enqueue 4 bytes (queued, WOULD_BLOCK), then a 4-byte group
     * (4+4 > 5) → transient WOULD_BLOCK, atomic (nothing reserved). */
    char four[4] = { 'a','a','a','a' };
    KlDatagramMessage mm = { .data = four, .len = 4, .peer = &p, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&b, &mm), KL_DATAGRAM_ACCEPTED);          /* queued (4 of 5 bytes used) */
    ASSERT_EQ((int)kl_dgram_send_queued(&b), 1);
    static char g4[4] = { 'z','z','z','z' };
    ASSERT_EQ(kl_dgram_send_enqueue_gso(&b, g4, 4, 2, 2, &p, -1, 0, NULL), KL_DATAGRAM_WOULD_BLOCK);
    ASSERT_EQ((int)kl_dgram_send_queued(&b), 1);                      /* atomic — group not reserved */
    ASSERT_EQ((int)kl_dgram_slots_free_count(&s2), 3);               /* only the 1 ordinary slot used */
    kl_dgram_send_discard_queued(&b);
    ASSERT_EQ(kl_dgram_send_free(&b), 0);
    kl_dgram_slots_free(&s2);
}

/* UNSUPPORTED on the whole-group submit latches the group to FALLBACK; its segments then drain as
 * ordinary per-segment sends (the single hook), in order. */
UTEST(dgram_send, gso_unsupported_then_fallback) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 8, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_DONE };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 0, KL_DGRAM_CAP_CONNECTED, 0, mock_submit, &mk), 0);
    kl_dgram_send_set_gso_cbs(&s, gso_submit, NULL, gso_done, NULL);
    g_gso_calls = g_gso_done_calls = 0; mk.order_n = 0;
    KlSockAddr p = any_peer();
    static char gU[6] = { 'U','U','U','U','U','U' };   /* 'U' → UNSUPPORTED on the whole-group submit */
    ASSERT_EQ(kl_dgram_send_enqueue_gso(&s, gU, 6, 2, 3, &p, -1, /*mode GSO*/0, NULL), KL_DATAGRAM_ACCEPTED);

    ASSERT_EQ(kl_dgram_send_flush(&s), 0);
    ASSERT_EQ(1, g_gso_calls);                          /* one whole-group attempt → UNSUPPORTED */
    ASSERT_EQ(3, mk.order_n);                            /* then 3 per-segment fallback sends */
    ASSERT_EQ(1, g_gso_done_calls);                      /* group retired once */
    ASSERT_EQ(0, g_gso_first == 0);                      /* the submit saw the buffer */
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 0);
    ASSERT_EQ((size_t)0, kl_dgram_send_dropped(&s));    /* fallback delivered — no drop */

    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* An arbitrary hard error on the whole-group submit drops the WHOLE group (recoverable) with NO
 * fallback retransmission. */
UTEST(dgram_send, gso_hard_error_drops_group) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 8, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_DONE };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 0, KL_DGRAM_CAP_CONNECTED, 0, mock_submit, &mk), 0);
    kl_dgram_send_set_gso_cbs(&s, gso_submit, NULL, gso_done, NULL);
    g_gso_calls = g_gso_done_calls = 0; mk.order_n = 0;
    KlSockAddr p = any_peer();
    static char gE[6] = { 'E','E','E','E','E','E' };   /* 'E' → hard ERROR on the whole-group submit */
    ASSERT_EQ(kl_dgram_send_enqueue_gso(&s, gE, 6, 2, 3, &p, -1, 0, NULL), KL_DATAGRAM_ACCEPTED);

    ASSERT_EQ(kl_dgram_send_flush(&s), 0);
    ASSERT_EQ(1, g_gso_calls);                          /* one whole-group attempt → hard error */
    ASSERT_EQ(0, mk.order_n);                            /* NO fallback retransmission */
    ASSERT_EQ((size_t)3, kl_dgram_send_dropped(&s));   /* the whole group (3 segments) dropped */
    ASSERT_EQ(0, kl_dgram_send_error(&s));              /* recoverable — NOT sticky */
    ASSERT_EQ(1, g_gso_done_calls);                      /* group retired (dropped) → done fired */
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 0);

    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* Connected-mode capability: a NULL peer GSO is UNSUPPORTED without KL_DGRAM_CAP_CONNECTED, ACCEPTED
 * with it. */
UTEST(dgram_send, gso_connected_capability) {
    KlAllocator a = kl_allocator_default();
    static char g4[4] = { '1','2','3','4' };

    KlDgramSlots s1; ASSERT_EQ(kl_dgram_slots_init(&s1, &a, 8, 64), 0);
    KlDgramSend no_conn;
    ASSERT_EQ(kl_dgram_send_init(&no_conn, &s1, &a, 0, /*caps*/0, 0, iso_submit, NULL), 0);
    ASSERT_EQ(kl_dgram_send_enqueue_gso(&no_conn, g4, 4, 2, 2, /*peer*/NULL, -1, 0, NULL),
              KL_DATAGRAM_UNSUPPORTED);                 /* connected send not granted */
    ASSERT_EQ((int)kl_dgram_send_queued(&no_conn), 0);
    ASSERT_EQ(kl_dgram_send_free(&no_conn), 0);
    kl_dgram_slots_free(&s1);

    KlDgramSlots s2; ASSERT_EQ(kl_dgram_slots_init(&s2, &a, 8, 64), 0);
    KlDgramSend conn;
    ASSERT_EQ(kl_dgram_send_init(&conn, &s2, &a, 0, KL_DGRAM_CAP_CONNECTED, 0, iso_submit, NULL), 0);
    ASSERT_EQ(kl_dgram_send_enqueue_gso(&conn, g4, 4, 2, 2, NULL, -1, 0, NULL), KL_DATAGRAM_ACCEPTED);
    kl_dgram_send_discard_queued(&conn);
    ASSERT_EQ(kl_dgram_send_free(&conn), 0);
    kl_dgram_slots_free(&s2);
}

UTEST_MAIN();
