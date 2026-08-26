/*
 * test_dgram_recv.c: serial receive + strict pause/resume over the inbound slot.
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
static unsigned     g_last_flags;
static KlDgramRecv *g_target;           /* the recv machine (for callback actions) */
enum { ACT_NONE = 0, ACT_PAUSE, ACT_STOP, ACT_RESUME };
static int          g_action, g_action_at;   /* perform g_action on delivery #g_action_at (1-based) */

static void on_deliver(void *ctx, const void *data, size_t len,
                       const KlSockAddr *peer, const KlSockAddr *local, unsigned flags) {
    (void)ctx;
    g_deliv++;
    if (len <= sizeof(g_last)) { memcpy(g_last, data, len); g_last_len = len; }
    g_has_peer   = (peer != NULL);
    g_has_local  = (local != NULL);
    g_last_flags = flags;
    if (g_deliv == g_action_at) {
        if (g_action == ACT_PAUSE)  kl_dgram_recv_pause(g_target);
        else if (g_action == ACT_STOP) kl_dgram_recv_stop(g_target);
        else if (g_action == ACT_RESUME) kl_dgram_recv_resume(g_target);
    }
}
static void reset_deliver(KlDgramRecv *r, int action, int at) {
    g_deliv = 0; g_last_len = 0; g_has_peer = g_has_local = 0; g_last_flags = 0;
    g_target = r; g_action = action; g_action_at = at;
}

/* Populate the inbound slot as a PROVIDER would, AFTER the machine has reset it (recv_arm/pull).
 * peer is MANDATORY; `with_local` sets a valid local + KL_DGRAM_HAS_LOCAL; `trunc` sets the provider
 * truncation hint (e.g. IOCP dwFlags at exact capacity). Copies up to the slot capacity. */
static void inbound_fill(KlDgramInbound *slots, const void *payload, size_t copy_len,
                         int with_local, int trunc) {
    KlDgramSlot *in = kl_dgram_inbound_slot(slots);
    kl_sockaddr_from_ipv4(&in->peer, (const unsigned char *)"\x7f\0\0\1", 12345);
    if (copy_len && payload) {
        size_t n = copy_len < in->cap ? copy_len : in->cap;
        memcpy(in->data, payload, n);
    }
    in->flags = 0;
    if (with_local) {
        kl_sockaddr_from_ipv4(&in->local, (const unsigned char *)"\xc0\xa8\x00\x01", 53);
        in->flags |= KL_DGRAM_HAS_LOCAL;
    }
    if (trunc) in->flags |= KL_DGRAM_TRUNCATED;
}

/* ── completion mock backend: arm = post; optionally inline-completes N datagrams ─────────────── */
typedef struct {
    KlDgramRecv  *r;
    KlDgramInbound *slots;
    int    posts;
    int    inline_remaining;   /* number of datagrams to deliver inline from arm() */
    size_t next_len;           /* reported length (may exceed in_cap → truncation-by-size) */
    const char *payload;
    int    with_local;         /* provide a valid local + KL_DGRAM_HAS_LOCAL */
    int    trunc;              /* set the provider truncation hint (exact-capacity truncation) */
    int    no_peer;            /* provider bug: deliver without a peer (contract violation) */
} CompCtx;
static int comp_arm(void *ctx) {
    CompCtx *c = ctx;
    c->posts++;
    if (c->inline_remaining > 0) {
        c->inline_remaining--;
        inbound_fill(c->slots, c->payload, c->next_len, c->with_local, c->trunc);
        if (c->no_peer) memset(&kl_dgram_inbound_slot(c->slots)->peer, 0,
                               sizeof(kl_dgram_inbound_slot(c->slots)->peer));
        kl_dgram_recv_on_complete(c->r, c->next_len, 1);   /* synchronous (inline) completion */
    }
    return 0;
}

/* ── readiness mock backend: arm/disarm interest + pull one datagram ─────────────────────────── */
typedef struct {
    KlDgramInbound *slots;
    int    arms, disarms, pulls;
    int    avail;              /* datagrams available to pull before would-block */
    const char *payload; size_t plen;
    size_t report_len;         /* if != 0, pull reports this length (else plen): truncation-by-size */
    int    with_local, trunc;  /* metadata knobs (as for CompCtx) */
    int    fatal;              /* if set, pull returns a fatal error (-1) */
} RdCtx;
static int  rd_arm(void *ctx)    { RdCtx *c = ctx; c->arms++;   return 0; }
static void rd_disarm(void *ctx) { RdCtx *c = ctx; c->disarms++; }
static int  rd_pull(void *ctx, size_t *out) {
    RdCtx *c = ctx;
    c->pulls++;
    if (c->fatal) return -1;                     /* fatal receive error */
    if (c->avail <= 0) return 0;                 /* would-block */
    c->avail--;
    inbound_fill(c->slots, c->payload, c->plen, c->with_local, c->trunc);
    *out = c->report_len ? c->report_len : c->plen;
    return 1;
}

/* ── inline-completion chain: iterative re-arm delivers each, no phantom, drains fully ────────── */
UTEST(dgram_recv, inline_completion_chain) {
    KlAllocator a = kl_allocator_default();
    KlDgramInbound slots; ASSERT_EQ(kl_dgram_inbound_init(&slots, &a, 64), 0);
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
    kl_dgram_recv_stop(&r);
    ASSERT_EQ(kl_dgram_recv_on_complete(&r, 0, 0), 0);     /* retire the outstanding post (dropped) */
    ASSERT_EQ(kl_dgram_recv_free(&r), 0);
    kl_dgram_inbound_free(&slots);
}

/* pause inside the delivery callback stops the inline chain (no further re-arm) */
UTEST(dgram_recv, pause_inside_delivery) {
    KlAllocator a = kl_allocator_default();
    KlDgramInbound slots; ASSERT_EQ(kl_dgram_inbound_init(&slots, &a, 64), 0);
    KlDgramRecv r;
    CompCtx c = { .slots = &slots, .inline_remaining = 5, .next_len = 1, .payload = "Z" };
    ASSERT_EQ(kl_dgram_recv_init(&r, &slots, 1, on_deliver, NULL, comp_arm, NULL, NULL, &c), 0);
    c.r = &r;
    reset_deliver(&r, ACT_PAUSE, 2);               /* pause on the 2nd delivery */
    ASSERT_EQ(kl_dgram_recv_start(&r), 0);
    ASSERT_EQ(g_deliv, 2);                          /* delivered 2, then paused → chain stops */
    ASSERT_EQ(kl_dgram_recv_inflight(&r), 0);       /* paused: nothing re-armed */
    kl_dgram_recv_free(&r);
    kl_dgram_inbound_free(&slots);
}

/* completion held-packet preservation: post, pause, complete→held, resume delivers once */
UTEST(dgram_recv, completion_held_then_resume) {
    KlAllocator a = kl_allocator_default();
    KlDgramInbound slots; ASSERT_EQ(kl_dgram_inbound_init(&slots, &a, 64), 0);
    KlDgramRecv r;
    CompCtx c = { .slots = &slots, .inline_remaining = 0, .next_len = 0, .payload = "" };
    ASSERT_EQ(kl_dgram_recv_init(&r, &slots, 1, on_deliver, NULL, comp_arm, NULL, NULL, &c), 0);
    c.r = &r;
    reset_deliver(&r, ACT_NONE, 0);
    ASSERT_EQ(kl_dgram_recv_start(&r), 0);          /* one recv posted (no inline) */
    ASSERT_EQ(kl_dgram_recv_inflight(&r), 1);

    kl_dgram_recv_pause(&r);                         /* strict pause; posted recv stays in flight */
    /* the posted recv completes while paused → held (data + metadata land in the inbound slot) */
    inbound_fill(&slots, "HELD", 4, /*with_local*/1, /*trunc*/0);
    ASSERT_EQ(kl_dgram_recv_on_complete(&r, 4, 1), 0);
    ASSERT_EQ(kl_dgram_recv_held(&r), 1);
    ASSERT_EQ(g_deliv, 0);                           /* not delivered while paused */

    ASSERT_EQ(kl_dgram_recv_resume(&r), 0);          /* delivers the held datagram exactly once */
    ASSERT_EQ(g_deliv, 1);
    ASSERT_EQ((int)g_last_len, 4);
    ASSERT_EQ(memcmp(g_last, "HELD", 4), 0);
    ASSERT_TRUE(g_has_peer);                          /* metadata snapshot preserved across pause/resume */
    ASSERT_TRUE(g_has_local);
    ASSERT_TRUE((g_last_flags & KL_DGRAM_HAS_LOCAL) != 0);
    ASSERT_TRUE((g_last_flags & KL_DGRAM_TRUNCATED) == 0);
    ASSERT_EQ(kl_dgram_recv_held(&r), 0);
    ASSERT_EQ(kl_dgram_recv_inflight(&r), 1);        /* re-armed after delivering the held datagram */
    kl_dgram_recv_stop(&r);
    kl_dgram_recv_on_complete(&r, 0, 0);
    kl_dgram_recv_free(&r);
    kl_dgram_inbound_free(&slots);
}

/* repeated pause/resume is idempotent and preserves the held datagram until the single delivery */
UTEST(dgram_recv, repeated_pause_resume_idempotent) {
    KlAllocator a = kl_allocator_default();
    KlDgramInbound slots; ASSERT_EQ(kl_dgram_inbound_init(&slots, &a, 64), 0);
    KlDgramRecv r;
    CompCtx c = { .slots = &slots, .inline_remaining = 0, .next_len = 0, .payload = "" };
    ASSERT_EQ(kl_dgram_recv_init(&r, &slots, 1, on_deliver, NULL, comp_arm, NULL, NULL, &c), 0);
    c.r = &r;
    reset_deliver(&r, ACT_NONE, 0);
    ASSERT_EQ(kl_dgram_recv_start(&r), 0);
    kl_dgram_recv_pause(&r); kl_dgram_recv_pause(&r);   /* idempotent */
    inbound_fill(&slots, "ONCE", 4, 0, 0);
    ASSERT_EQ(kl_dgram_recv_on_complete(&r, 4, 1), 0);  /* held */
    ASSERT_EQ(kl_dgram_recv_resume(&r), 0);
    ASSERT_EQ(kl_dgram_recv_resume(&r), 0);             /* idempotent: no second delivery */
    ASSERT_EQ(g_deliv, 1);
    kl_dgram_recv_stop(&r);
    kl_dgram_recv_on_complete(&r, 0, 0);
    kl_dgram_recv_free(&r);
    kl_dgram_inbound_free(&slots);
}

/* duplicate/spurious completion is dropped without disturbing held data */
UTEST(dgram_recv, duplicate_completion_dropped) {
    KlAllocator a = kl_allocator_default();
    KlDgramInbound slots; ASSERT_EQ(kl_dgram_inbound_init(&slots, &a, 64), 0);
    KlDgramRecv r;
    CompCtx c = { .slots = &slots, .inline_remaining = 0, .next_len = 0, .payload = "" };
    ASSERT_EQ(kl_dgram_recv_init(&r, &slots, 1, on_deliver, NULL, comp_arm, NULL, NULL, &c), 0);
    c.r = &r;
    reset_deliver(&r, ACT_NONE, 0);
    ASSERT_EQ(kl_dgram_recv_start(&r), 0);
    kl_dgram_recv_pause(&r);
    inbound_fill(&slots, "KEEP", 4, 0, 0);
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
    kl_dgram_inbound_free(&slots);
}

/* ── uniform truncation + metadata conformance ──────────────────────────────────────────────── */
#define PAY16 "0123456789ABCDEF"   /* exactly 16 bytes (== in_cap below) */

/* a completion recv machine with in_cap 16, armed (async, no inline) and ready for one manual
 * completion. Retire the trailing re-armed op + free with fin_recv(). */
static void start_comp16(KlDgramRecv *r, KlDgramInbound *slots, KlAllocator *a, CompCtx *c) {
    kl_dgram_inbound_init(slots, a, 16);   /* fixed valid args → 0 (asserted via later behavior) */
    *c = (CompCtx){ .slots = slots };
    kl_dgram_recv_init(r, slots, 1, on_deliver, NULL, comp_arm, NULL, NULL, c);
    c->r = r; reset_deliver(r, ACT_NONE, 0);
    kl_dgram_recv_start(r);
}
static void fin_recv(KlDgramRecv *r, KlDgramInbound *slots) {   /* retire the trailing re-armed op + free */
    if (kl_dgram_recv_inflight(r)) { kl_dgram_recv_stop(r); kl_dgram_recv_on_complete(r, 0, 0); }
    kl_dgram_recv_free(r);
    kl_dgram_inbound_free(slots);
}

/* exact-capacity datagram fills the buffer WITHOUT truncation */
UTEST(dgram_recv, exact_capacity_no_truncation) {
    KlAllocator a = kl_allocator_default(); KlDgramInbound slots; KlDgramRecv r; CompCtx c;
    start_comp16(&r, &slots, &a, &c);
    inbound_fill(&slots, PAY16, 16, 0, 0);
    ASSERT_EQ(kl_dgram_recv_on_complete(&r, 16, 1), 0);        /* reported == in_cap */
    ASSERT_EQ(g_deliv, 1);
    ASSERT_EQ((int)g_last_len, 16);                            /* full payload delivered */
    ASSERT_TRUE((g_last_flags & KL_DGRAM_TRUNCATED) == 0);     /* not truncated */
    ASSERT_TRUE(g_has_peer);
    ASSERT_TRUE(!kl_dgram_recv_error(&r));
    fin_recv(&r, &slots);
}

/* oversized-by-length datagram → captured prefix + KL_DGRAM_TRUNCATED, NOT a fatal receive */
UTEST(dgram_recv, oversized_length_truncated_not_fatal) {
    KlAllocator a = kl_allocator_default(); KlDgramInbound slots; KlDgramRecv r; CompCtx c;
    start_comp16(&r, &slots, &a, &c);
    inbound_fill(&slots, PAY16, 16, 0, 0);                     /* slot holds its 16-byte capacity */
    ASSERT_EQ(kl_dgram_recv_on_complete(&r, 999, 1), 0);       /* reported > in_cap → truncated */
    ASSERT_EQ(g_deliv, 1);
    ASSERT_EQ((int)g_last_len, 16);                            /* captured prefix = in_cap (never more) */
    ASSERT_TRUE((g_last_flags & KL_DGRAM_TRUNCATED) != 0);
    ASSERT_TRUE(!kl_dgram_recv_error(&r));                     /* truncation is not an error */
    fin_recv(&r, &slots);
}

/* exact-capacity + a provider truncation HINT (IOCP WSAMSG.dwFlags / WSAEMSGSIZE): TRUNCATED even
 * though the byte count equals capacity */
UTEST(dgram_recv, exact_capacity_flag_truncated) {
    KlAllocator a = kl_allocator_default(); KlDgramInbound slots; KlDgramRecv r; CompCtx c;
    start_comp16(&r, &slots, &a, &c);
    inbound_fill(&slots, PAY16, 16, /*local*/0, /*trunc*/1);   /* dwFlags MSG_TRUNC at exact capacity */
    ASSERT_EQ(kl_dgram_recv_on_complete(&r, 16, 1), 0);
    ASSERT_EQ((int)g_last_len, 16);
    ASSERT_TRUE((g_last_flags & KL_DGRAM_TRUNCATED) != 0);     /* truncation via the flag, not the length */
    fin_recv(&r, &slots);
}

/* readiness: an oversized pull is delivered truncated (not fatal), interest stays armed */
UTEST(dgram_recv, oversized_readiness_truncated_not_fatal) {
    KlAllocator a = kl_allocator_default();
    KlDgramInbound slots; ASSERT_EQ(kl_dgram_inbound_init(&slots, &a, 16), 0);
    KlDgramRecv r;
    RdCtx rd = { .slots = &slots, .avail = 1, .payload = PAY16, .plen = 16, .report_len = 999 };
    ASSERT_EQ(kl_dgram_recv_init(&r, &slots, 0, on_deliver, NULL, rd_arm, rd_disarm, rd_pull, &rd), 0);
    reset_deliver(&r, ACT_NONE, 0);
    ASSERT_EQ(kl_dgram_recv_start(&r), 0);
    ASSERT_EQ(kl_dgram_recv_on_readable(&r), 0);              /* delivers 1, then would-block: NOT -1 */
    ASSERT_EQ(g_deliv, 1);
    ASSERT_EQ((int)g_last_len, 16);
    ASSERT_TRUE((g_last_flags & KL_DGRAM_TRUNCATED) != 0);
    ASSERT_EQ(rd.disarms, 0);                                 /* truncation does not disarm the source */
    ASSERT_TRUE(!kl_dgram_recv_error(&r));
    kl_dgram_recv_stop(&r);
    ASSERT_EQ(kl_dgram_recv_free(&r), 0);
    kl_dgram_inbound_free(&slots);
}

/* a genuine zero-length datagram is delivered and stays DISTINCT from a receive failure */
UTEST(dgram_recv, zero_length_distinct_from_failure) {
    KlAllocator a = kl_allocator_default(); KlDgramInbound slots; KlDgramRecv r; CompCtx c;
    start_comp16(&r, &slots, &a, &c);
    inbound_fill(&slots, NULL, 0, 0, 0);                       /* peer set, empty payload */
    ASSERT_EQ(kl_dgram_recv_on_complete(&r, 0, 1), 0);         /* ok, len 0 → a real datagram */
    ASSERT_EQ(g_deliv, 1);
    ASSERT_EQ((int)g_last_len, 0);
    ASSERT_TRUE(g_has_peer);
    ASSERT_TRUE((g_last_flags & KL_DGRAM_TRUNCATED) == 0);
    ASSERT_TRUE(!kl_dgram_recv_error(&r));
    /* contrast: ok=0 on the re-armed op is a FAILURE: no delivery, error set */
    ASSERT_EQ(kl_dgram_recv_on_complete(&r, 0, 0), -1);
    ASSERT_EQ(g_deliv, 1);                                     /* no second delivery */
    ASSERT_TRUE(kl_dgram_recv_error(&r));
    ASSERT_EQ(kl_dgram_recv_free(&r), 0);
    kl_dgram_inbound_free(&slots);
}

/* peer is mandatory: a successful completion without a source is a provider contract violation */
UTEST(dgram_recv, peer_mandatory_missing_fails_safe) {
    KlAllocator a = kl_allocator_default(); KlDgramInbound slots; KlDgramRecv r; CompCtx c;
    start_comp16(&r, &slots, &a, &c);
    inbound_fill(&slots, "hi", 2, 0, 0);
    memset(&kl_dgram_inbound_slot(&slots)->peer, 0,          /* strip the peer (provider bug) */
           sizeof(kl_dgram_inbound_slot(&slots)->peer));
    ASSERT_EQ(kl_dgram_recv_on_complete(&r, 2, 1), -1);       /* fails safe: no anonymous delivery */
    ASSERT_EQ(g_deliv, 0);
    ASSERT_TRUE(kl_dgram_recv_error(&r));
    ASSERT_EQ(kl_dgram_recv_inflight(&r), 0);
    ASSERT_EQ(kl_dgram_recv_free(&r), 0);
    kl_dgram_inbound_free(&slots);
}

/* local is delivered only with an explicit HAS_LOCAL; a local without the flag is ignored AND a
 * prior packet's local never survives into the next receive */
UTEST(dgram_recv, local_absent_and_stale_cleared) {
    KlAllocator a = kl_allocator_default(); KlDgramInbound slots; KlDgramRecv r; CompCtx c;
    start_comp16(&r, &slots, &a, &c);
    inbound_fill(&slots, "P1", 2, /*with_local*/1, 0);        /* packet 1 carries a valid local */
    ASSERT_EQ(kl_dgram_recv_on_complete(&r, 2, 1), 0);        /* delivered + re-armed (slot reset) */
    ASSERT_TRUE(g_has_local);
    ASSERT_TRUE((g_last_flags & KL_DGRAM_HAS_LOCAL) != 0);
    /* packet 2: provider writes a local address but forgets HAS_LOCAL → machine must ignore + clear */
    inbound_fill(&slots, "P2", 2, /*with_local*/0, 0);
    kl_sockaddr_from_ipv4(&kl_dgram_inbound_slot(&slots)->local,
                          (const unsigned char *)"\x0a\x00\x00\x01", 7);
    ASSERT_EQ(kl_dgram_recv_on_complete(&r, 2, 1), 0);
    ASSERT_EQ(g_deliv, 2);
    ASSERT_TRUE(!g_has_local);                                /* NOT leaked from packet 1, NOT the raw addr */
    ASSERT_TRUE((g_last_flags & KL_DGRAM_HAS_LOCAL) == 0);
    fin_recv(&r, &slots);
}

/* an oversized datagram received while PAUSED is held and delivered (truncated) on resume, with its
 * complete metadata snapshot preserved */
UTEST(dgram_recv, paused_truncated_held_then_resume) {
    KlAllocator a = kl_allocator_default(); KlDgramInbound slots; KlDgramRecv r; CompCtx c;
    start_comp16(&r, &slots, &a, &c);
    kl_dgram_recv_pause(&r);
    inbound_fill(&slots, PAY16, 16, /*with_local*/1, /*trunc*/0);
    ASSERT_EQ(kl_dgram_recv_on_complete(&r, 999, 1), 0);      /* oversized while paused → held */
    ASSERT_EQ(kl_dgram_recv_held(&r), 1);
    ASSERT_EQ(g_deliv, 0);
    ASSERT_EQ(kl_dgram_recv_resume(&r), 0);                   /* delivered once on resume */
    ASSERT_EQ(g_deliv, 1);
    ASSERT_EQ((int)g_last_len, 16);                           /* captured prefix */
    ASSERT_TRUE((g_last_flags & KL_DGRAM_TRUNCATED) != 0);    /* truncation snapshot preserved */
    ASSERT_TRUE((g_last_flags & KL_DGRAM_HAS_LOCAL) != 0);    /* local snapshot preserved */
    ASSERT_TRUE(g_has_local);
    fin_recv(&r, &slots);
}

/* an active failed completion is NOT delivered as an empty datagram (error state, no re-arm) */
UTEST(dgram_recv, active_failed_completion_not_delivered) {
    KlAllocator a = kl_allocator_default();
    KlDgramInbound slots; ASSERT_EQ(kl_dgram_inbound_init(&slots, &a, 64), 0);
    KlDgramRecv r;
    CompCtx c = { .slots = &slots, .inline_remaining = 0, .next_len = 0, .payload = "" };
    ASSERT_EQ(kl_dgram_recv_init(&r, &slots, 1, on_deliver, NULL, comp_arm, NULL, NULL, &c), 0);
    c.r = &r;
    reset_deliver(&r, ACT_NONE, 0);
    ASSERT_EQ(kl_dgram_recv_start(&r), 0);
    ASSERT_EQ(kl_dgram_recv_inflight(&r), 1);
    ASSERT_EQ(kl_dgram_recv_on_complete(&r, 0, 0), -1);     /* failed receive */
    ASSERT_EQ(g_deliv, 0);                                  /* NOT delivered as a 0-length datagram */
    ASSERT_TRUE(kl_dgram_recv_error(&r));
    ASSERT_EQ(kl_dgram_recv_inflight(&r), 0);               /* retired; no re-arm */
    ASSERT_EQ(kl_dgram_recv_free(&r), 0);
    kl_dgram_inbound_free(&slots);
}

/* a failed completion while paused is NOT a held packet and is never delivered on resume */
UTEST(dgram_recv, paused_failed_completion_not_held) {
    KlAllocator a = kl_allocator_default();
    KlDgramInbound slots; ASSERT_EQ(kl_dgram_inbound_init(&slots, &a, 64), 0);
    KlDgramRecv r;
    CompCtx c = { .slots = &slots, .inline_remaining = 0, .next_len = 0, .payload = "" };
    ASSERT_EQ(kl_dgram_recv_init(&r, &slots, 1, on_deliver, NULL, comp_arm, NULL, NULL, &c), 0);
    c.r = &r;
    reset_deliver(&r, ACT_NONE, 0);
    ASSERT_EQ(kl_dgram_recv_start(&r), 0);
    kl_dgram_recv_pause(&r);
    ASSERT_EQ(kl_dgram_recv_on_complete(&r, 0, 0), -1);     /* failed while paused */
    ASSERT_EQ(kl_dgram_recv_held(&r), 0);                   /* NOT held */
    ASSERT_TRUE(kl_dgram_recv_error(&r));
    ASSERT_EQ(kl_dgram_recv_resume(&r), 0);
    ASSERT_EQ(g_deliv, 0);                                  /* nothing delivered on resume */
    ASSERT_EQ(kl_dgram_recv_inflight(&r), 0);               /* stopped: no re-arm */
    ASSERT_EQ(kl_dgram_recv_free(&r), 0);
    kl_dgram_inbound_free(&slots);
}

/* readiness fatal pull disarms exactly once, leaves nothing armed, and frees cleanly */
UTEST(dgram_recv, readiness_fatal_pull) {
    KlAllocator a = kl_allocator_default();
    KlDgramInbound slots; ASSERT_EQ(kl_dgram_inbound_init(&slots, &a, 64), 0);
    KlDgramRecv r;
    RdCtx rd = { .slots = &slots, .fatal = 1, .payload = "x", .plen = 1 };
    ASSERT_EQ(kl_dgram_recv_init(&r, &slots, 0, on_deliver, NULL, rd_arm, rd_disarm, rd_pull, &rd), 0);
    reset_deliver(&r, ACT_NONE, 0);
    ASSERT_EQ(kl_dgram_recv_start(&r), 0);
    ASSERT_EQ(kl_dgram_recv_on_readable(&r), -1);           /* fatal pull */
    ASSERT_EQ(g_deliv, 0);
    ASSERT_EQ(rd.disarms, 1);                               /* disarmed exactly once */
    ASSERT_EQ(kl_dgram_recv_inflight(&r), 0);              /* not left armed */
    ASSERT_TRUE(kl_dgram_recv_error(&r));
    kl_dgram_recv_stop(&r);                                  /* idempotent */
    ASSERT_EQ(rd.disarms, 1);
    ASSERT_EQ(kl_dgram_recv_free(&r), 0);                   /* not wedged */
    kl_dgram_inbound_free(&slots);
}

/* readiness: serial drain, pause disarms interest, resume re-arms */
UTEST(dgram_recv, readiness_serial_drain_and_disarm) {
    KlAllocator a = kl_allocator_default();
    KlDgramInbound slots; ASSERT_EQ(kl_dgram_inbound_init(&slots, &a, 64), 0);
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
    kl_dgram_inbound_free(&slots);
}

/* readiness: pause from inside the delivery callback stops the serial drain immediately */
UTEST(dgram_recv, readiness_pause_inside_delivery) {
    KlAllocator a = kl_allocator_default();
    KlDgramInbound slots; ASSERT_EQ(kl_dgram_inbound_init(&slots, &a, 64), 0);
    KlDgramRecv r;
    RdCtx rd = { .slots = &slots, .avail = 5, .payload = "D", .plen = 1 };
    ASSERT_EQ(kl_dgram_recv_init(&r, &slots, 0, on_deliver, NULL, rd_arm, rd_disarm, rd_pull, &rd), 0);
    reset_deliver(&r, ACT_PAUSE, 2);                        /* pause on 2nd delivery */
    ASSERT_EQ(kl_dgram_recv_start(&r), 0);
    ASSERT_EQ(kl_dgram_recv_on_readable(&r), 0);
    ASSERT_EQ(g_deliv, 2);                                  /* stopped draining after the pause */
    ASSERT_EQ(rd.disarms, 1);
    kl_dgram_recv_free(&r);
    kl_dgram_inbound_free(&slots);
}

/* free is refused while a receive is outstanding (references the inbound slot) */
UTEST(dgram_recv, free_refused_while_inflight) {
    KlAllocator a = kl_allocator_default();
    KlDgramInbound slots; ASSERT_EQ(kl_dgram_inbound_init(&slots, &a, 64), 0);
    KlDgramRecv r;
    CompCtx c = { .slots = &slots, .inline_remaining = 0, .next_len = 0, .payload = "" };
    ASSERT_EQ(kl_dgram_recv_init(&r, &slots, 1, on_deliver, NULL, comp_arm, NULL, NULL, &c), 0);
    c.r = &r;
    reset_deliver(&r, ACT_NONE, 0);
    ASSERT_EQ(kl_dgram_recv_start(&r), 0);
    ASSERT_EQ(kl_dgram_recv_inflight(&r), 1);
    ASSERT_EQ(kl_dgram_recv_free(&r), -1);                  /* refused: recv references inbound slot */
    kl_dgram_recv_stop(&r);
    ASSERT_EQ(kl_dgram_recv_on_complete(&r, 0, 0), 0);      /* posted op retires (dropped) */
    ASSERT_EQ(kl_dgram_recv_free(&r), 0);                   /* now safe */
    kl_dgram_inbound_free(&slots);
}

UTEST_MAIN();
