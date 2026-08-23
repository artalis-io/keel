/*
 * test_stream.c — internal KlStream write machinery (src/stream_write.h).
 * Exercised in isolation with mock readiness-writer + completion-submit hooks (no live sockets).
 */
#include "utest.h"
#include <keel/http_connection.h>
#include <keel/allocator.h>
#include "../src/stream_write.h"
#include "../src/drain_reserve.h"   /* low-water knobs for the write-during-writable test */
#include <string.h>

/* ── Mock readiness writer ───────────────────────────────────────────────
 * mode: 0 = accept all, 1 = accept half (partial), 2 = would-block (0), 3 = error (-1). */
typedef struct { char buf[16384]; size_t len; int mode; int calls; int switch_after; } RW;
static void rw_init(RW *w) { memset(w, 0, sizeof(*w)); w->switch_after = -1; }
static kl_ssize_t rw_write(const char *data, size_t len, void *ctx) {
    RW *w = ctx; w->calls++;
    int mode = (w->switch_after >= 0 && w->calls > w->switch_after) ? 0 : w->mode;
    if (mode == 3) return -1;
    if (mode == 2) return 0;
    size_t acc = (mode == 1) ? (len / 2) : len;
    if (acc == 0 && len > 0) acc = 1;
    if (w->len + acc > sizeof(w->buf)) return -1;
    memcpy(w->buf + w->len, data, acc); w->len += acc;
    return (kl_ssize_t)acc;
}

/* ── Mock completion submitter ───────────────────────────────────────────
 * Records each submitted batch in order; `fail` makes submit return -1. */
typedef struct { char buf[16384]; size_t total; int submits; int fail; size_t last_len; } CS;
static void cs_init(CS *c) { memset(c, 0, sizeof(*c)); }
static int cs_submit(void *ctx, const char *data, size_t len) {
    CS *c = ctx; c->submits++;
    if (c->fail) return -1;
    if (c->total + len > sizeof(c->buf)) return -1;
    memcpy(c->buf + c->total, data, len); c->total += len; c->last_len = len;
    return 0;
}

/* ── Readiness ───────────────────────────────────────────────────────────── */

UTEST(stream_write, readiness_partial_send_then_flush_in_order) {
    KlAllocator a = kl_allocator_default();
    RW w; rw_init(&w); w.mode = 1;   /* accept half on the direct send */
    KlStream s; memset(&s, 0, sizeof(s));
    ASSERT_EQ(kl_stream_write_init(&s, &a, 64), 0);
    kl_stream_set_writer(&s, rw_write, &w);

    char src[7] = "abcdef";
    ASSERT_EQ((int)kl_stream_write(&s, src, 6), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)w.len, 3);                       /* prefix "abc" sent inline */
    ASSERT_EQ((int)kl_stream_write_pending(&s), 3); /* remainder buffered */
    memset(src, 'Z', sizeof(src));                  /* caller buffer mutated — remainder copied */

    w.mode = 0;                                     /* accept everything on flush */
    ASSERT_EQ(kl_stream_flush(&s), 0);
    ASSERT_EQ((int)w.len, 6);
    ASSERT_EQ(memcmp(w.buf, "abcdef", 6), 0);       /* original bytes, in order */
    ASSERT_EQ((int)kl_stream_write_pending(&s), 0);

    kl_stream_write_free(&s);
}

UTEST(stream_write, capacity_boundaries) {
    KlAllocator a = kl_allocator_default();
    RW w; rw_init(&w); w.mode = 2;   /* would-block: everything buffers into the reserved queue */
    KlStream s; memset(&s, 0, sizeof(s));
    ASSERT_EQ(kl_stream_write_init(&s, &a, 8), 0);
    kl_stream_set_writer(&s, rw_write, &w);

    ASSERT_EQ((int)kl_stream_write(&s, "123456789", 9), (int)KL_STREAM_TOO_LARGE);  /* > cap 8 */
    ASSERT_EQ((int)kl_stream_write_pending(&s), 0);
    ASSERT_EQ((int)kl_stream_write(&s, "hello", 5), (int)KL_STREAM_ACCEPTED);       /* buffered 5 */
    ASSERT_EQ((int)kl_stream_write(&s, "wxyz", 4), (int)KL_STREAM_WOULD_BLOCK);     /* 5+4 > 8 */
    ASSERT_EQ((int)kl_stream_write_pending(&s), 5);                                 /* unchanged */
    ASSERT_EQ((int)kl_stream_write(&s, "xyz", 3), (int)KL_STREAM_ACCEPTED);         /* fills to 8 */
    ASSERT_EQ((int)kl_stream_write_pending(&s), 8);

    kl_stream_write_free(&s);
}

/* ── Completion ──────────────────────────────────────────────────────────── */

UTEST(stream_write, completion_one_send_in_flight_then_pump) {
    KlAllocator a = kl_allocator_default();
    CS c; cs_init(&c);
    KlStream s; memset(&s, 0, sizeof(s));
    ASSERT_EQ(kl_stream_write_init(&s, &a, 64), 0);
    kl_stream_set_submit(&s, cs_submit, &c, /*copying=*/0);   /* referencing backend */

    ASSERT_EQ((int)kl_stream_write(&s, "hello", 5), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ(c.submits, 1);                       /* one submit posted */
    ASSERT_EQ((int)c.last_len, 5);
    ASSERT_EQ((int)kl_stream_write_pending(&s), 5);/* referencing: still queued until completion */

    /* A second write while a send is in flight must NOT post another (ordering). */
    ASSERT_EQ((int)kl_stream_write(&s, "world", 5), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ(c.submits, 1);                       /* still one — blocked by send_inflight */
    ASSERT_EQ((int)kl_stream_write_pending(&s), 10);

    /* WRITE completion: consume the delivered bytes, then pump the next batch. */
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);
    ASSERT_EQ(c.submits, 2);
    ASSERT_EQ((int)c.last_len, 5);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0); /* nothing left to pump */
    ASSERT_EQ(c.submits, 2);
    ASSERT_EQ((int)kl_stream_write_pending(&s), 0);
    ASSERT_EQ((int)c.total, 10);
    ASSERT_EQ(memcmp(c.buf, "helloworld", 10), 0); /* order preserved */

    kl_stream_write_free(&s);
}

UTEST(stream_write, completion_copying_consumes_at_submit_but_blocks_next) {
    KlAllocator a = kl_allocator_default();
    CS c; cs_init(&c);
    KlStream s; memset(&s, 0, sizeof(s));
    ASSERT_EQ(kl_stream_write_init(&s, &a, 64), 0);
    kl_stream_set_submit(&s, cs_submit, &c, /*copying=*/1);   /* copying backend */

    char src[6] = "hello";
    ASSERT_EQ((int)kl_stream_write(&s, src, 5), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ(c.submits, 1);
    ASSERT_EQ((int)kl_drain_buffered(&s.wq), 0);   /* copying: queue consumed at submit */
    ASSERT_EQ((int)kl_stream_write_pending(&s), 5);/* but still pending (unacked in flight) */
    memset(src, 'Z', sizeof(src));                 /* caller buffer mutated after submit — safe */

    /* Next write buffers but does not submit (ordering blocked until completion). */
    ASSERT_EQ((int)kl_stream_write(&s, "world", 5), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ(c.submits, 1);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);  /* completion → pump "world" */
    ASSERT_EQ(c.submits, 2);
    ASSERT_EQ((int)c.total, 10);
    ASSERT_EQ(memcmp(c.buf, "helloworld", 10), 0);

    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);  /* ack in-flight "world" (write_free refuses
                                                        * while a send is in flight) */
    kl_stream_write_free(&s);
}

UTEST(stream_write, completion_submission_failure_keeps_bytes_and_errors) {
    KlAllocator a = kl_allocator_default();
    CS c; cs_init(&c); c.fail = 1;                  /* submit always fails */
    KlStream s; memset(&s, 0, sizeof(s));
    ASSERT_EQ(kl_stream_write_init(&s, &a, 64), 0);
    kl_stream_set_submit(&s, cs_submit, &c, 0);

    ASSERT_EQ((int)kl_stream_write(&s, "hello", 5), (int)KL_STREAM_ERROR);  /* submit failed */
    ASSERT_EQ(c.submits, 1);
    ASSERT_EQ((int)c.total, 0);                     /* nothing delivered */
    ASSERT_EQ((int)kl_stream_write_pending(&s), 5); /* bytes still owned — never discarded */

    /* Sticky: subsequent writes fail, no further submits. */
    ASSERT_EQ((int)kl_stream_write(&s, "x", 1), (int)KL_STREAM_ERROR);
    ASSERT_EQ(c.submits, 1);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), -1);

    kl_stream_write_free(&s);
}

UTEST(stream_write, completion_write_failure_referencing_retains_bytes) {
    /* A WRITE that FAILS after a successful submit — referencing backend must NOT
     * consume the queued bytes (provider may still reference them) and must not pump. */
    KlAllocator a = kl_allocator_default();
    CS c; cs_init(&c);
    KlStream s; memset(&s, 0, sizeof(s));
    ASSERT_EQ(kl_stream_write_init(&s, &a, 64), 0);
    kl_stream_set_submit(&s, cs_submit, &c, /*copying=*/0);

    ASSERT_EQ((int)kl_stream_write(&s, "hello", 5), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)kl_stream_write(&s, "world", 5), (int)KL_STREAM_ACCEPTED);  /* queued behind */
    ASSERT_EQ(c.submits, 1);

    ASSERT_EQ(kl_stream_on_write_complete(&s, /*ok=*/0), -1);   /* delivery failed */
    ASSERT_EQ(c.submits, 1);                          /* did NOT pump the next batch */
    ASSERT_EQ((int)kl_stream_write_pending(&s), 10);  /* referencing: bytes retained */
    ASSERT_EQ((int)kl_stream_write(&s, "x", 1), (int)KL_STREAM_ERROR);  /* sticky */
    ASSERT_EQ(kl_stream_flush(&s), -1);

    kl_stream_write_free(&s);
}

UTEST(stream_write, completion_write_failure_copying) {
    /* Copying backend: the in-flight copy was retired at submit; a failed WRITE still errors,
     * does not pump, and leaves no queued bytes. */
    KlAllocator a = kl_allocator_default();
    CS c; cs_init(&c);
    KlStream s; memset(&s, 0, sizeof(s));
    ASSERT_EQ(kl_stream_write_init(&s, &a, 64), 0);
    kl_stream_set_submit(&s, cs_submit, &c, /*copying=*/1);

    ASSERT_EQ((int)kl_stream_write(&s, "hello", 5), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)kl_drain_buffered(&s.wq), 0);      /* consumed at submit */
    ASSERT_EQ(kl_stream_on_write_complete(&s, /*ok=*/0), -1);
    ASSERT_EQ((int)kl_stream_write_pending(&s), 0);   /* copy retired, delivery failed */
    ASSERT_EQ((int)kl_stream_write(&s, "x", 1), (int)KL_STREAM_ERROR);  /* sticky */

    kl_stream_write_free(&s);
}

UTEST(stream_write, free_refused_while_send_in_flight) {
    /* Freeing while a send is outstanding would UAF provider-referenced storage. */
    KlAllocator a = kl_allocator_default();
    CS c; cs_init(&c);
    KlStream s; memset(&s, 0, sizeof(s));
    ASSERT_EQ(kl_stream_write_init(&s, &a, 64), 0);
    kl_stream_set_submit(&s, cs_submit, &c, /*copying=*/0);

    ASSERT_EQ((int)kl_stream_write(&s, "hello", 5), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ(kl_stream_write_free(&s), -1);          /* refused — op outstanding */
    ASSERT_EQ((int)kl_stream_write_pending(&s), 5);   /* intact */

    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0); /* op retired */
    ASSERT_EQ(kl_stream_write_free(&s), 0);           /* now safe */
}

UTEST(stream_write, free_refused_while_send_in_flight_copying) {
    KlAllocator a = kl_allocator_default();
    CS c; cs_init(&c);
    KlStream s; memset(&s, 0, sizeof(s));
    ASSERT_EQ(kl_stream_write_init(&s, &a, 64), 0);
    kl_stream_set_submit(&s, cs_submit, &c, /*copying=*/1);
    ASSERT_EQ((int)kl_stream_write(&s, "hello", 5), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ(kl_stream_write_free(&s), -1);          /* copying still holds the completion target */
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);
    ASSERT_EQ(kl_stream_write_free(&s), 0);
}

UTEST(stream_write, write_without_transport_hook_fails_closed) {
    /* Readiness mode with no writer installed must not deref a NULL write_fn. */
    KlAllocator a = kl_allocator_default();
    KlStream s; memset(&s, 0, sizeof(s));
    ASSERT_EQ(kl_stream_write_init(&s, &a, 64), 0);   /* no set_writer / set_submit */

    ASSERT_EQ((int)kl_stream_write(&s, "hello", 5), (int)KL_STREAM_ERROR);
    ASSERT_EQ((int)kl_stream_write_pending(&s), 0);   /* nothing taken */
    ASSERT_EQ(s.wq_err, 0);                           /* not sticky — a setup ordering issue */

    /* Installing the writer afterward makes writes work (proves non-sticky). */
    RW w; rw_init(&w); w.mode = 0;
    ASSERT_EQ(kl_stream_set_writer(&s, rw_write, &w), 0);
    ASSERT_EQ((int)kl_stream_write(&s, "hi", 2), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)w.len, 2);

    kl_stream_write_free(&s);
}

UTEST(stream_write, flush_fails_closed_in_completion_and_without_writer) {
    KlAllocator a = kl_allocator_default();

    /* Completion mode with queued data: flush must fail closed (no synchronous write_fn). */
    CS c; cs_init(&c);
    KlStream s; memset(&s, 0, sizeof(s));
    ASSERT_EQ(kl_stream_write_init(&s, &a, 64), 0);
    kl_stream_set_submit(&s, cs_submit, &c, /*copying=*/0);
    ASSERT_EQ((int)kl_stream_write(&s, "hello", 5), (int)KL_STREAM_ACCEPTED);  /* in flight */
    ASSERT_EQ((int)kl_stream_write(&s, "world", 5), (int)KL_STREAM_ACCEPTED);  /* queued */
    ASSERT_EQ(kl_stream_flush(&s), -1);            /* completion mode → refused, no mutation */
    ASSERT_EQ(c.submits, 1);                       /* untouched */
    ASSERT_EQ((int)kl_stream_write_pending(&s), 10);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);  /* acks "hello", pumps "world" */
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);  /* acks "world" — nothing left in flight */
    kl_stream_write_free(&s);

    /* Readiness mode with no writer installed: flush must not deref a NULL write_fn. */
    KlStream r; memset(&r, 0, sizeof(r));
    ASSERT_EQ(kl_stream_write_init(&r, &a, 64), 0);   /* no set_writer */
    ASSERT_EQ(kl_stream_flush(&r), -1);               /* fails closed, no crash */
    kl_stream_write_free(&r);
}

UTEST(stream_write, config_locked_while_data_active) {
    /* Transport mode/ownership must not change while data is queued or in flight. */
    KlAllocator a = kl_allocator_default();
    RW w; rw_init(&w); w.mode = 2;   /* would-block → bytes stay queued */
    CS c; cs_init(&c);
    KlStream s; memset(&s, 0, sizeof(s));
    ASSERT_EQ(kl_stream_write_init(&s, &a, 64), 0);
    ASSERT_EQ(kl_stream_set_writer(&s, rw_write, &w), 0);
    ASSERT_EQ((int)kl_stream_write(&s, "hello", 5), (int)KL_STREAM_ACCEPTED);  /* queued */

    ASSERT_EQ(kl_stream_set_submit(&s, cs_submit, &c, 1), -1);  /* rejected — bytes queued */
    ASSERT_EQ(kl_stream_set_writer(&s, rw_write, &w), -1);      /* rejected — bytes queued */

    w.mode = 0;
    ASSERT_EQ(kl_stream_flush(&s), 0);                          /* drained */
    ASSERT_EQ(kl_stream_set_writer(&s, rw_write, &w), 0);       /* now allowed (idle) */

    kl_stream_write_free(&s);
}

/* ── Reentrancy ──────────────────────────────────────────────────────────── */

typedef struct { KlStream *s; const char *data; size_t len; int fired; } WriteCbCtx;
static void write_on_writable(void *ctx) {
    WriteCbCtx *w = ctx;
    w->fired++;
    kl_stream_write(w->s, w->data, w->len);   /* write from inside the writable callback */
}

UTEST(stream_write, write_during_writable_callback) {
    KlAllocator a = kl_allocator_default();
    RW w; rw_init(&w); w.mode = 2;   /* buffer everything first */
    KlStream s; memset(&s, 0, sizeof(s));
    ASSERT_EQ(kl_stream_write_init(&s, &a, 64), 0);
    kl_stream_set_writer(&s, rw_write, &w);

    WriteCbCtx cb = { .s = &s, .data = "more", .len = 4, .fired = 0 };
    kl_drain_set_low_water(&s.wq, 2);
    kl_drain_on_writable(&s.wq, write_on_writable, &cb);

    ASSERT_EQ((int)kl_stream_write(&s, "0123456789", 10), (int)KL_STREAM_ACCEPTED);  /* buffered 10 */

    /* Flush with an accepting writer: drains to empty, crosses low-water → the callback
     * writes "more", which (buffer now empty) is sent inline by the same accepting writer. */
    w.mode = 0;
    ASSERT_EQ(kl_stream_flush(&s), 0);
    ASSERT_EQ(cb.fired, 1);
    ASSERT_EQ(memcmp(w.buf, "0123456789more", 14), 0);   /* original then callback bytes, in order */
    ASSERT_EQ((int)kl_stream_write_pending(&s), 0);

    kl_stream_write_free(&s);
}

UTEST(stream_write, init_rejects_double_and_frees_safely) {
    KlAllocator a = kl_allocator_default();
    KlStream s; memset(&s, 0, sizeof(s));
    ASSERT_EQ(kl_stream_write_init(&s, &a, 32), 0);
    ASSERT_EQ(kl_stream_write_init(&s, &a, 32), -1);   /* already inited */
    kl_stream_write_free(&s);
    kl_stream_write_free(&s);                          /* idempotent */
}

UTEST_MAIN();
