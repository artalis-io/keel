/*
 * test_dgram_recv.c — Phase B step 3: serial receive + strict pause/resume over the inbound slot.
 *
 * Covers: inline-completion chains (iterative, no phantom), pause inside delivery, repeated
 * pause/resume idempotence, held-packet preservation, duplicate/spurious completion dropped without
 * disturbing held data, capacity bounds (fail-safe), readiness disarm on pause + serial drain, and
 * inbound/outbound storage independence.
 */
#include "utest.h"

#include "../src/datagram_recv.h"

#include <string.h>

/* ── delivery recorder (+ optional pause/stop action at a chosen delivery index) ─────────────── */
static int          g_deliv;
static unsigned char g_last[256];
static size_t       g_last_len;
static int          g_has_peer, g_has_local;
static KlDgramRecv *g_target;           /* the recv machine (for callback actions) */
enum { ACT_NONE = 0, ACT_PAUSE, ACT_STOP, ACT_RESUME };
static int          g_action, g_action_at;   /* perform g_action on delivery #g_action_at (1-based) */

static void on_deliver(void *ctx, const void *data, size_t len,
                       const KlSockAddr *peer, const KlSockAddr *local, unsigned flags) {
    (void)ctx; (void)flags;
    g_deliv++;
    if (len <= sizeof(g_last)) { memcpy(g_last, data, len); g_last_len = len; }
    g_has_peer  = (peer != NULL);
    g_has_local = (local != NULL);
    if (g_deliv == g_action_at) {
        if (g_action == ACT_PAUSE)  kl_dgram_recv_pause(g_target);
        else if (g_action == ACT_STOP) kl_dgram_recv_stop(g_target);
        else if (g_action == ACT_RESUME) kl_dgram_recv_resume(g_target);
    }
}
static void reset_deliver(KlDgramRecv *r, int action, int at) {
    g_deliv = 0; g_last_len = 0; g_has_peer = g_has_local = 0;
    g_target = r; g_action = action; g_action_at = at;
}

/* ── completion mock backend: arm = post; optionally inline-completes N datagrams ─────────────── */
typedef struct {
    KlDgramRecv  *r;
    KlDgramSlots *slots;
    int    posts;
    int    inline_remaining;   /* number of datagrams to deliver inline from arm() */
    size_t next_len;
    const char *payload;
} CompCtx;
static int comp_arm(void *ctx) {
    CompCtx *c = ctx;
    c->posts++;
    if (c->inline_remaining > 0) {
        c->inline_remaining--;
        KlDgramSlot *in = kl_dgram_slots_inbound(c->slots);
        memcpy(in->data, c->payload, c->next_len);
        kl_dgram_recv_on_complete(c->r, c->next_len, 1);   /* synchronous (inline) completion */
    }
    return 0;
}

/* ── readiness mock backend: arm/disarm interest + pull one datagram ─────────────────────────── */
typedef struct {
    KlDgramSlots *slots;
    int    arms, disarms, pulls;
    int    avail;              /* datagrams available to pull before would-block */
    const char *payload; size_t plen;
    size_t bad_len;            /* if != 0, pull reports this length (capacity-bound test) */
} RdCtx;
static int  rd_arm(void *ctx)    { RdCtx *c = ctx; c->arms++;   return 0; }
static void rd_disarm(void *ctx) { RdCtx *c = ctx; c->disarms++; }
static int  rd_pull(void *ctx, size_t *out) {
    RdCtx *c = ctx;
    c->pulls++;
    if (c->avail <= 0) return 0;                 /* would-block */
    c->avail--;
    KlDgramSlot *in = kl_dgram_slots_inbound(c->slots);
    size_t n = c->bad_len ? c->bad_len : c->plen;
    if (!c->bad_len) memcpy(in->data, c->payload, c->plen);
    *out = n;
    return 1;
}

/* ── inline-completion chain: iterative re-arm delivers each, no phantom, drains fully ────────── */
UTEST(dgram_recv, inline_completion_chain) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 4, 64, 64), 0);
    KlDgramRecv r;
    CompCtx c = { .slots = &slots, .inline_remaining = 3, .next_len = 4, .payload = "PKT!" };
    ASSERT_EQ(kl_dgram_recv_init(&r, &slots, /*completion*/1, on_deliver, NULL,
                                 comp_arm, NULL, NULL, &c), 0);
    c.r = &r;
    reset_deliver(&r, ACT_NONE, 0);
    ASSERT_EQ(kl_dgram_recv_start(&r), 0);
    ASSERT_EQ(g_deliv, 3);                         /* all three inline datagrams delivered */
    ASSERT_EQ((int)g_last_len, 4);
    ASSERT_EQ(memcmp(g_last, "PKT!", 4), 0);
    ASSERT_EQ(kl_dgram_recv_inflight(&r), 1);      /* the 4th arm (no inline) is posted, awaiting */
    ASSERT_EQ((int)kl_dgram_slots_free_count(&slots), 4);  /* recv never touches outbound slots */
    kl_dgram_recv_stop(&r);
    ASSERT_EQ(kl_dgram_recv_on_complete(&r, 0, 0), 0);     /* retire the outstanding post (dropped) */
    ASSERT_EQ(kl_dgram_recv_free(&r), 0);
    kl_dgram_slots_free(&slots);
}

/* pause inside the delivery callback stops the inline chain (no further re-arm) */
UTEST(dgram_recv, pause_inside_delivery) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 2, 64, 64), 0);
    KlDgramRecv r;
    CompCtx c = { .slots = &slots, .inline_remaining = 5, .next_len = 1, .payload = "Z" };
    ASSERT_EQ(kl_dgram_recv_init(&r, &slots, 1, on_deliver, NULL, comp_arm, NULL, NULL, &c), 0);
    c.r = &r;
    reset_deliver(&r, ACT_PAUSE, 2);               /* pause on the 2nd delivery */
    ASSERT_EQ(kl_dgram_recv_start(&r), 0);
    ASSERT_EQ(g_deliv, 2);                          /* delivered 2, then paused → chain stops */
    ASSERT_EQ(kl_dgram_recv_inflight(&r), 0);       /* paused: nothing re-armed */
    kl_dgram_recv_free(&r);
    kl_dgram_slots_free(&slots);
}

/* completion held-packet preservation: post, pause, complete→held, resume delivers once */
UTEST(dgram_recv, completion_held_then_resume) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 2, 64, 64), 0);
    KlDgramRecv r;
    CompCtx c = { .slots = &slots, .inline_remaining = 0, .next_len = 0, .payload = "" };
    ASSERT_EQ(kl_dgram_recv_init(&r, &slots, 1, on_deliver, NULL, comp_arm, NULL, NULL, &c), 0);
    c.r = &r;
    reset_deliver(&r, ACT_NONE, 0);
    ASSERT_EQ(kl_dgram_recv_start(&r), 0);          /* one recv posted (no inline) */
    ASSERT_EQ(kl_dgram_recv_inflight(&r), 1);

    kl_dgram_recv_pause(&r);                         /* strict pause; posted recv stays in flight */
    /* the posted recv completes while paused → held (data lands in the inbound slot) */
    KlDgramSlot *in = kl_dgram_slots_inbound(&slots);
    memcpy(in->data, "HELD", 4);
    ASSERT_EQ(kl_dgram_recv_on_complete(&r, 4, 1), 0);
    ASSERT_EQ(kl_dgram_recv_held(&r), 1);
    ASSERT_EQ(g_deliv, 0);                           /* not delivered while paused */

    ASSERT_EQ(kl_dgram_recv_resume(&r), 0);          /* delivers the held datagram exactly once */
    ASSERT_EQ(g_deliv, 1);
    ASSERT_EQ((int)g_last_len, 4);
    ASSERT_EQ(memcmp(g_last, "HELD", 4), 0);
    ASSERT_EQ(kl_dgram_recv_held(&r), 0);
    ASSERT_EQ(kl_dgram_recv_inflight(&r), 1);        /* re-armed after delivering the held datagram */
    kl_dgram_recv_stop(&r);
    kl_dgram_recv_on_complete(&r, 0, 0);
    kl_dgram_recv_free(&r);
    kl_dgram_slots_free(&slots);
}

/* repeated pause/resume is idempotent and preserves the held datagram until the single delivery */
UTEST(dgram_recv, repeated_pause_resume_idempotent) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 2, 64, 64), 0);
    KlDgramRecv r;
    CompCtx c = { .slots = &slots, .inline_remaining = 0, .next_len = 0, .payload = "" };
    ASSERT_EQ(kl_dgram_recv_init(&r, &slots, 1, on_deliver, NULL, comp_arm, NULL, NULL, &c), 0);
    c.r = &r;
    reset_deliver(&r, ACT_NONE, 0);
    ASSERT_EQ(kl_dgram_recv_start(&r), 0);
    kl_dgram_recv_pause(&r); kl_dgram_recv_pause(&r);   /* idempotent */
    KlDgramSlot *in = kl_dgram_slots_inbound(&slots);
    memcpy(in->data, "ONCE", 4);
    ASSERT_EQ(kl_dgram_recv_on_complete(&r, 4, 1), 0);  /* held */
    ASSERT_EQ(kl_dgram_recv_resume(&r), 0);
    ASSERT_EQ(kl_dgram_recv_resume(&r), 0);             /* idempotent — no second delivery */
    ASSERT_EQ(g_deliv, 1);
    kl_dgram_recv_stop(&r);
    kl_dgram_recv_on_complete(&r, 0, 0);
    kl_dgram_recv_free(&r);
    kl_dgram_slots_free(&slots);
}

/* duplicate/spurious completion is dropped without disturbing held data */
UTEST(dgram_recv, duplicate_completion_dropped) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 2, 64, 64), 0);
    KlDgramRecv r;
    CompCtx c = { .slots = &slots, .inline_remaining = 0, .next_len = 0, .payload = "" };
    ASSERT_EQ(kl_dgram_recv_init(&r, &slots, 1, on_deliver, NULL, comp_arm, NULL, NULL, &c), 0);
    c.r = &r;
    reset_deliver(&r, ACT_NONE, 0);
    ASSERT_EQ(kl_dgram_recv_start(&r), 0);
    kl_dgram_recv_pause(&r);
    KlDgramSlot *in = kl_dgram_slots_inbound(&slots);
    memcpy(in->data, "KEEP", 4);
    ASSERT_EQ(kl_dgram_recv_on_complete(&r, 4, 1), 0);   /* held */
    ASSERT_EQ(kl_dgram_recv_held(&r), 1);
    /* a second (spurious) completion with nothing in flight must be dropped, held data untouched */
    ASSERT_EQ(kl_dgram_recv_on_complete(&r, 9, 1), 0);
    ASSERT_EQ(kl_dgram_recv_held(&r), 1);
    ASSERT_EQ(kl_dgram_recv_resume(&r), 0);
    ASSERT_EQ(g_deliv, 1);
    ASSERT_EQ((int)g_last_len, 4);                        /* the ORIGINAL held datagram, not the spurious 9 */
    ASSERT_EQ(memcmp(g_last, "KEEP", 4), 0);
    kl_dgram_recv_stop(&r);
    kl_dgram_recv_on_complete(&r, 0, 0);
    kl_dgram_recv_free(&r);
    kl_dgram_slots_free(&slots);
}

/* capacity bound: an over-capacity provider length fails safely (stopped, not delivered) */
UTEST(dgram_recv, capacity_bound_fails_safe) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 2, 16, 16), 0);   /* in_cap 16 */
    /* completion */
    KlDgramRecv rc;
    CompCtx c = { .slots = &slots, .inline_remaining = 0, .next_len = 0, .payload = "" };
    ASSERT_EQ(kl_dgram_recv_init(&rc, &slots, 1, on_deliver, NULL, comp_arm, NULL, NULL, &c), 0);
    c.r = &rc;
    reset_deliver(&rc, ACT_NONE, 0);
    ASSERT_EQ(kl_dgram_recv_start(&rc), 0);
    ASSERT_EQ(kl_dgram_recv_on_complete(&rc, 999, 1), -1);   /* > in_cap → fail safe */
    ASSERT_EQ(g_deliv, 0);
    kl_dgram_recv_free(&rc);
    /* readiness */
    KlDgramRecv rr;
    RdCtx rd = { .slots = &slots, .avail = 1, .payload = "x", .plen = 1, .bad_len = 999 };
    ASSERT_EQ(kl_dgram_recv_init(&rr, &slots, 0, on_deliver, NULL, rd_arm, rd_disarm, rd_pull, &rd), 0);
    reset_deliver(&rr, ACT_NONE, 0);
    ASSERT_EQ(kl_dgram_recv_start(&rr), 0);
    ASSERT_EQ(kl_dgram_recv_on_readable(&rr), -1);          /* pull len > in_cap → fail safe */
    ASSERT_EQ(g_deliv, 0);
    kl_dgram_recv_free(&rr);
    kl_dgram_slots_free(&slots);
}

/* readiness: serial drain, pause disarms interest, resume re-arms */
UTEST(dgram_recv, readiness_serial_drain_and_disarm) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 2, 64, 64), 0);
    KlDgramRecv r;
    RdCtx rd = { .slots = &slots, .avail = 3, .payload = "D", .plen = 1 };
    ASSERT_EQ(kl_dgram_recv_init(&r, &slots, 0, on_deliver, NULL, rd_arm, rd_disarm, rd_pull, &rd), 0);
    reset_deliver(&r, ACT_NONE, 0);
    ASSERT_EQ(kl_dgram_recv_start(&r), 0);
    ASSERT_EQ(rd.arms, 1);
    ASSERT_EQ(kl_dgram_recv_on_readable(&r), 0);            /* drains 3, then would-block */
    ASSERT_EQ(g_deliv, 3);
    ASSERT_EQ(rd.disarms, 0);

    kl_dgram_recv_pause(&r);
    ASSERT_EQ(rd.disarms, 1);                               /* pause drops READ interest */
    ASSERT_EQ(kl_dgram_recv_inflight(&r), 0);
    rd.avail = 2;
    ASSERT_EQ(kl_dgram_recv_on_readable(&r), 0);            /* paused: no pull/deliver */
    ASSERT_EQ(g_deliv, 3);

    ASSERT_EQ(kl_dgram_recv_resume(&r), 0);
    ASSERT_EQ(rd.arms, 2);                                  /* resume re-arms interest */
    ASSERT_EQ(kl_dgram_recv_on_readable(&r), 0);            /* drains the 2 now available */
    ASSERT_EQ(g_deliv, 5);
    kl_dgram_recv_stop(&r);
    ASSERT_EQ(kl_dgram_recv_free(&r), 0);
    kl_dgram_slots_free(&slots);
}

/* readiness: pause from inside the delivery callback stops the serial drain immediately */
UTEST(dgram_recv, readiness_pause_inside_delivery) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 2, 64, 64), 0);
    KlDgramRecv r;
    RdCtx rd = { .slots = &slots, .avail = 5, .payload = "D", .plen = 1 };
    ASSERT_EQ(kl_dgram_recv_init(&r, &slots, 0, on_deliver, NULL, rd_arm, rd_disarm, rd_pull, &rd), 0);
    reset_deliver(&r, ACT_PAUSE, 2);                        /* pause on 2nd delivery */
    ASSERT_EQ(kl_dgram_recv_start(&r), 0);
    ASSERT_EQ(kl_dgram_recv_on_readable(&r), 0);
    ASSERT_EQ(g_deliv, 2);                                  /* stopped draining after the pause */
    ASSERT_EQ(rd.disarms, 1);
    kl_dgram_recv_free(&r);
    kl_dgram_slots_free(&slots);
}

/* free is refused while a receive is outstanding (references the inbound slot) */
UTEST(dgram_recv, free_refused_while_inflight) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots slots; ASSERT_EQ(kl_dgram_slots_init(&slots, &a, 2, 64, 64), 0);
    KlDgramRecv r;
    CompCtx c = { .slots = &slots, .inline_remaining = 0, .next_len = 0, .payload = "" };
    ASSERT_EQ(kl_dgram_recv_init(&r, &slots, 1, on_deliver, NULL, comp_arm, NULL, NULL, &c), 0);
    c.r = &r;
    reset_deliver(&r, ACT_NONE, 0);
    ASSERT_EQ(kl_dgram_recv_start(&r), 0);
    ASSERT_EQ(kl_dgram_recv_inflight(&r), 1);
    ASSERT_EQ(kl_dgram_recv_free(&r), -1);                  /* refused — recv references inbound slot */
    kl_dgram_recv_stop(&r);
    ASSERT_EQ(kl_dgram_recv_on_complete(&r, 0, 0), 0);      /* posted op retires (dropped) */
    ASSERT_EQ(kl_dgram_recv_free(&r), 0);                   /* now safe */
    kl_dgram_slots_free(&slots);
}

UTEST_MAIN();
