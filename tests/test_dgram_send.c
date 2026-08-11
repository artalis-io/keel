/*
 * test_dgram_send.c — Phase B step 2: atomic send machinery over KlDgramSlots.
 *
 * Covers: exact status mapping (ACCEPTED/WOULD_BLOCK/TOO_LARGE/UNSUPPORTED/CLOSED/ERROR), refusal
 * takes no ownership, copy-before-return, FIFO order over the LIFO free-slot allocator, bounded
 * in-flight, sticky submit failure (never discards), on_writable (full→non-full) / on_drain
 * (non-empty→empty) edges, reentrant callbacks observing updated state, teardown refusal while
 * in-flight, and an allocation-free hot path.
 */
#include "utest.h"

#include "../src/datagram_send.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Counting allocator (real malloc/free + call counters) — for the allocation-free hot path ── */
static int g_mallocs, g_frees;
static void *cnt_malloc(void *c, size_t n)                  { (void)c; g_mallocs++; return malloc(n); }
static void *cnt_realloc(void *c, void *p, size_t o, size_t n){ (void)c; (void)o; return realloc(p, n); }
static void  cnt_free(void *c, void *p, size_t s)          { (void)c; (void)s; g_frees++; free(p); }
static KlAllocator counting_alloc(void) {
    KlAllocator a = { cnt_malloc, cnt_realloc, cnt_free, NULL };
    return a;
}

/* ── Mock submit provider ─────────────────────────────────────────────────────────────────── */
typedef struct {
    KlDgramSubmitResult next;    /* value the next submit returns */
    int    calls;                /* total submit calls */
    unsigned char last[2048];    /* copy of the last submitted payload */
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

/* Callback counters. */
static int g_writable, g_drain;
static void on_writable_cb(void *ctx) { (void)ctx; g_writable++; }
static void on_drain_cb(void *ctx)    { (void)ctx; g_drain++; }

static KlSockAddr any_peer(void) {
    KlSockAddr a; kl_sockaddr_from_ipv4(&a, (const unsigned char *)"\x7f\0\0\1", 9999);
    return a;
}

/* ── synchronous (readiness) provider: direct fast path, no slot churn, no on_drain spam ─────── */
UTEST(dgram_send, readiness_sync_direct) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_DONE };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, /*completion*/0, /*max_inflight*/1,
                                 KL_DGRAM_CAP_CONNECTED, mock_submit, &mk), 0);
    kl_dgram_send_set_drain_cb(&s, on_drain_cb, NULL);
    g_drain = 0;
    KlSockAddr p = any_peer();
    for (int i = 0; i < 10; i++) {
        KlDatagramMessage m = { .data = "hello", .len = 5, .peer = &p, .tos = -1 };
        ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);
        ASSERT_EQ((int)kl_dgram_send_queued(&s), 0);           /* direct — nothing queued */
    }
    ASSERT_EQ(mk.calls, 10);
    ASSERT_EQ(g_drain, 0);                                     /* no transient non-empty→empty */
    ASSERT_EQ((int)kl_dgram_slots_free_count(&slots), 4);      /* no slot consumed */
    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* readiness WOULD_BLOCK queues (copy), flush drains, on_drain fires once on empty */
UTEST(dgram_send, readiness_wouldblock_then_flush) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_WOULDBLOCK };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 0, 1, KL_DGRAM_CAP_CONNECTED, mock_submit, &mk), 0);
    kl_dgram_send_set_drain_cb(&s, on_drain_cb, NULL);
    g_drain = 0;
    KlSockAddr p = any_peer();
    char buf[5]; memcpy(buf, "AAAAA", 5);
    KlDatagramMessage m = { .data = buf, .len = 5, .peer = &p, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 1);               /* queued on WOULD_BLOCK */

    memset(buf, 'Z', 5);                                       /* scribble source: slot must hold a copy */
    mk.next = KL_DGRAM_SUBMIT_DONE;
    ASSERT_EQ(kl_dgram_send_flush(&s), 0);
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 0);
    ASSERT_EQ((int)mk.last_len, 5);
    ASSERT_EQ(memcmp(mk.last, "AAAAA", 5), 0);                 /* copied at send, not referenced */
    ASSERT_EQ(g_drain, 1);                                     /* non-empty→empty once */
    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* completion async: send queues + submits (INFLIGHT), on_complete retires, on_drain on empty */
UTEST(dgram_send, completion_async_retire) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, /*completion*/1, 1, KL_DGRAM_CAP_CONNECTED, mock_submit, &mk), 0);
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

/* FIFO submit order despite a scrambled (LIFO) free-slot allocator */
UTEST(dgram_send, fifo_order_over_lifo_slots) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64, 64), 0);

    /* Scramble the free-list: acquire all, release in an order that makes the LIFO stack hand out
     * non-sequential slots next. */
    KlDgramSlot *t[4];
    for (int i = 0; i < 4; i++) t[i] = kl_dgram_slots_acquire(&slots);
    kl_dgram_slots_release(&slots, t[1]);
    kl_dgram_slots_release(&slots, t[3]);
    kl_dgram_slots_release(&slots, t[0]);
    kl_dgram_slots_release(&slots, t[2]);

    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, /*max_inflight*/1, KL_DGRAM_CAP_CONNECTED, mock_submit, &mk), 0);
    KlSockAddr p = any_peer();
    /* Send A,B,C,D (distinct first byte). max_inflight 1 → submitted one at a time across completes. */
    const char *msgs[4] = { "A...", "B...", "C...", "D..." };
    for (int i = 0; i < 4; i++) {
        KlDatagramMessage m = { .data = msgs[i], .len = 4, .peer = &p, .tos = -1 };
        ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);
    }
    /* Drain: each complete lets the next be submitted. */
    for (int i = 0; i < 4; i++) ASSERT_EQ(kl_dgram_send_on_complete(&s, 1), 0);
    ASSERT_EQ(mk.order_n, 4);
    ASSERT_EQ(mk.order[0], 'A');   /* FIFO — not the LIFO slot order */
    ASSERT_EQ(mk.order[1], 'B');
    ASSERT_EQ(mk.order[2], 'C');
    ASSERT_EQ(mk.order[3], 'D');
    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* full→non-full fires on_writable exactly once; refusal (WOULD_BLOCK) takes no ownership */
UTEST(dgram_send, writable_edge_and_no_ownership_on_refusal) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 2, 64, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, /*max_inflight*/4, KL_DGRAM_CAP_CONNECTED, mock_submit, &mk), 0);
    kl_dgram_send_set_writable_cb(&s, on_writable_cb, NULL);
    g_writable = 0;
    KlSockAddr p = any_peer();
    KlDatagramMessage m = { .data = "x", .len = 1, .peer = &p, .tos = -1 };

    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);
    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);    /* fills both slots */
    ASSERT_EQ((int)kl_dgram_slots_free_count(&slots), 0);

    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_WOULD_BLOCK); /* refused */
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 2);              /* refusal: no ownership taken */
    ASSERT_EQ((int)kl_dgram_slots_free_count(&slots), 0);    /* no queue mutation */

    ASSERT_EQ(kl_dgram_send_on_complete(&s, 1), 0);           /* one retires → full→non-full */
    ASSERT_EQ(g_writable, 1);
    ASSERT_EQ(kl_dgram_send_on_complete(&s, 1), 0);           /* not full → no second edge */
    ASSERT_EQ(g_writable, 1);
    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* status mapping: TOO_LARGE / UNSUPPORTED / CLOSED / ERROR(bad args), none take ownership */
UTEST(dgram_send, status_mapping_and_no_ownership) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 2, 16, 16), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, 2, /*caps*/0, mock_submit, &mk), 0);
    KlSockAddr p = any_peer(), lo = any_peer();

    /* TOO_LARGE: exceeds the 16-byte slot. */
    char big[32]; memset(big, 'B', sizeof(big));
    KlDatagramMessage tl = { .data = big, .len = 32, .peer = &p, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &tl), KL_DATAGRAM_TOO_LARGE);

    /* UNSUPPORTED: source-pin / per-packet TOS / connected-mode without the caps. */
    KlDatagramMessage sp = { .data = "x", .len = 1, .peer = &p, .local = &lo, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &sp), KL_DATAGRAM_UNSUPPORTED);
    KlDatagramMessage tos = { .data = "x", .len = 1, .peer = &p, .tos = 4 };
    ASSERT_EQ(kl_dgram_send(&s, &tos), KL_DATAGRAM_UNSUPPORTED);
    KlDatagramMessage conn = { .data = "x", .len = 1, .peer = NULL, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &conn), KL_DATAGRAM_UNSUPPORTED);

    /* ERROR: bad args (len without data). */
    KlDatagramMessage bad = { .data = NULL, .len = 4, .peer = &p, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &bad), KL_DATAGRAM_ERROR);

    /* CLOSED after closing. */
    kl_dgram_send_set_closing(&s, 1);
    KlDatagramMessage ok = { .data = "x", .len = 1, .peer = &p, .tos = -1 };
    ASSERT_EQ(kl_dgram_send(&s, &ok), KL_DATAGRAM_CLOSED);

    /* None of the refusals took ownership or mutated the queue. */
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
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, 2, KL_DGRAM_CAP_CONNECTED, mock_submit, &mk), 0);
    KlSockAddr p = any_peer();
    KlDatagramMessage m = { .data = "keep", .len = 4, .peer = &p, .tos = -1 };

    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);   /* accepted+queued; submit then failed */
    ASSERT_TRUE(kl_dgram_send_error(&s) != 0);               /* sticky error set */
    ASSERT_EQ((int)kl_dgram_send_queued(&s), 1);             /* retained — not discarded */
    ASSERT_EQ((int)kl_dgram_send_inflight(&s), 0);           /* the failed submit did not go in-flight */
    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ERROR);     /* subsequent sends refuse (sticky) */

    ASSERT_EQ(kl_dgram_send_free(&s), 0);                    /* nothing in flight → free allowed */
    kl_dgram_slots_free(&slots);
}

/* reentrant callback observes fully-updated state (a free slot is available) */
static KlDgramSend *g_re_s;
static int          g_re_sent;
static int          g_re_status;   /* result of the reentrant send (checked in the test body) */
static size_t       g_re_free_seen;/* free slots visible to the reentrant callback */
static void reentrant_writable(void *ctx) {
    (void)ctx;
    if (g_re_sent) return;
    g_re_sent = 1;
    /* A slot was just freed before this callback fired — observe fully-updated state, then send. */
    g_re_free_seen = kl_dgram_slots_free_count(g_re_s->slots);
    KlSockAddr p = any_peer();
    KlDatagramMessage m = { .data = "R", .len = 1, .peer = &p, .tos = -1 };
    g_re_status = (int)kl_dgram_send(g_re_s, &m);
}
UTEST(dgram_send, reentrant_callback_sees_updated_state) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 2, 32, 32), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, 4, KL_DGRAM_CAP_CONNECTED, mock_submit, &mk), 0);
    g_re_s = &s; g_re_sent = 0; g_re_status = -1; g_re_free_seen = 0;
    kl_dgram_send_set_writable_cb(&s, reentrant_writable, NULL);
    KlSockAddr p = any_peer();
    KlDatagramMessage m = { .data = "x", .len = 1, .peer = &p, .tos = -1 };

    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);
    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);   /* full (2 slots) */
    ASSERT_EQ((int)kl_dgram_slots_free_count(&slots), 0);
    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_WOULD_BLOCK);/* arms full */

    /* Completing one frees a slot and fires on_writable, which reentrantly sends into it. */
    ASSERT_EQ(kl_dgram_send_on_complete(&s, 1), 0);
    ASSERT_EQ(g_re_sent, 1);
    ASSERT_TRUE(g_re_free_seen >= 1);                        /* callback saw the freed slot */
    ASSERT_EQ(g_re_status, KL_DATAGRAM_ACCEPTED);            /* reentrant send succeeded */
    ASSERT_TRUE(kl_dgram_send_queued(&s) >= 1);              /* reentrant send took the freed slot */

    /* Drain everything so nothing is in flight, then free. */
    while (kl_dgram_send_inflight(&s) > 0) kl_dgram_send_on_complete(&s, 1);
    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

/* teardown refusal: free is refused while a provider-referenced send is outstanding */
UTEST(dgram_send, free_refused_while_inflight) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 2, 32, 32), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, 2, KL_DGRAM_CAP_CONNECTED, mock_submit, &mk), 0);
    KlSockAddr p = any_peer();
    KlDatagramMessage m = { .data = "x", .len = 1, .peer = &p, .tos = -1 };

    ASSERT_EQ(kl_dgram_send(&s, &m), KL_DATAGRAM_ACCEPTED);
    ASSERT_EQ((int)kl_dgram_send_inflight(&s), 1);
    ASSERT_EQ(kl_dgram_send_free(&s), -1);                   /* refused — slots referenced by provider */

    ASSERT_EQ(kl_dgram_send_on_complete(&s, 1), 0);
    ASSERT_EQ(kl_dgram_send_free(&s), 0);                    /* now safe */
    kl_dgram_slots_free(&slots);
}

/* the send/complete/flush hot path performs no allocation */
UTEST(dgram_send, hot_path_is_allocation_free) {
    KlAllocator a = counting_alloc();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64, 64), 0);
    Mock mk = { .next = KL_DGRAM_SUBMIT_INFLIGHT };
    KlDgramSend s;
    ASSERT_EQ(kl_dgram_send_init(&s, &slots, &a, 1, 4, KL_DGRAM_CAP_CONNECTED, mock_submit, &mk), 0);
    int base = g_mallocs;   /* snapshot after all init-time allocation for `s` */
    KlSockAddr p = any_peer();
    for (int round = 0; round < 100; round++) {
        KlDatagramMessage m = { .data = "payload", .len = 7, .peer = &p, .tos = -1 };
        (void)kl_dgram_send(&s, &m);
        kl_dgram_send_on_complete(&s, 1);
    }
    ASSERT_EQ(g_mallocs, base);    /* no allocation on the completion send/complete hot path */

    /* readiness queue + flush path (init a second machine — its ring alloc is init-time). */
    Mock mk2 = { .next = KL_DGRAM_SUBMIT_WOULDBLOCK };
    KlDgramSend r;
    ASSERT_EQ(kl_dgram_send_init(&r, &slots, &a, 0, 1, KL_DGRAM_CAP_CONNECTED, mock_submit, &mk2), 0);
    int base2 = g_mallocs;        /* snapshot after `r`'s init-time ring allocation */
    for (int i = 0; i < 4; i++) {
        KlDatagramMessage m = { .data = "q", .len = 1, .peer = &p, .tos = -1 };
        (void)kl_dgram_send(&r, &m);
    }
    mk2.next = KL_DGRAM_SUBMIT_DONE;
    kl_dgram_send_flush(&r);
    ASSERT_EQ(g_mallocs, base2);   /* no allocation on the readiness queue+flush hot path */

    ASSERT_EQ(kl_dgram_send_free(&r), 0);
    ASSERT_EQ(kl_dgram_send_free(&s), 0);
    kl_dgram_slots_free(&slots);
}

UTEST_MAIN();
