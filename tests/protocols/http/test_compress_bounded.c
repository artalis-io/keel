/* Bounded compressed streaming: kl_http_compress_stream_try_write / _try_end over a codec with
 * KlCompress.flush_bound. A mock codec makes every byte visible; the response runs over a fake
 * socket provider whose send takes everything or nothing, as in test_http_response.c. */
#include "utest.h"
#include <keel/compress.h>
#include <keel/http_compress.h>
#include <keel/http_response.h>
#include <keel/event_ctx.h>
#include <keel/socket.h>
#include "event_caps.h"
#include "http_response_internal.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Mock codec: "C:" + input per feed, "|S" per sync flush, "[END]" on finish ──────────────── */

typedef struct {
    KlCompress base;
    int feeds;              /* feed() calls */
    int last_flush;
    int destroyed;
    size_t fed;             /* input bytes taken: the codec state a refused write must not touch */
    size_t overshoot;       /* emit this many bytes past the bound (a defective codec) */
    int huge_bound;         /* flush_bound answers SIZE_MAX */
} BMock;

static size_t bmock_bound(KlCompress *self, size_t len, int final) {
    BMock *m = (BMock *)self;
    if (m->huge_bound) return SIZE_MAX;
    return 2 + len + 2 + (final ? 5 : 0);
}
static int bmock_feed(KlCompress *self, const char *data, size_t len, int flush,
                      int (*emit)(void *ctx, const char *data, size_t len), void *emit_ctx) {
    BMock *m = (BMock *)self;
    m->feeds++;
    m->last_flush = flush;
    m->fed += len;
    if (data && len) {
        if (emit(emit_ctx, "C:", 2) < 0 || emit(emit_ctx, data, len) < 0) return -1;
    }
    if (flush == KL_COMPRESS_FLUSH_SYNC && emit(emit_ctx, "|S", 2) < 0) return -1;
    if (flush == 1 && emit(emit_ctx, "[END]", 5) < 0) return -1;
    for (size_t i = 0; i < m->overshoot; i++)
        if (emit(emit_ctx, "!", 1) < 0) return -1;
    return 0;
}
static int bmock_compress(KlCompress *self, const char *in, size_t in_len, char **out,
                          size_t *out_len, KlAllocator *alloc) {
    (void)self; (void)in; (void)in_len; (void)out; (void)out_len; (void)alloc;
    return -1;
}
static const char *bmock_encoding(KlCompress *self) { (void)self; return "mock"; }
static void bmock_destroy(KlCompress *self) { ((BMock *)self)->destroyed = 1; }

static BMock g_m;
static int g_no_bound;      /* the factory builds a codec without flush_bound */

static KlCompress *bmock_factory(KlCompressCtx *ctx, KlAllocator *alloc) {
    (void)ctx; (void)alloc;
    memset(&g_m, 0, sizeof g_m);
    g_m.base.compress = bmock_compress;
    g_m.base.feed = bmock_feed;
    g_m.base.encoding = bmock_encoding;
    g_m.base.destroy = bmock_destroy;
    if (!g_no_bound) g_m.base.flush_bound = bmock_bound;
    return &g_m.base;
}

/* ── A response over a gate: send takes everything (open) or nothing ─────────────────────────── */

typedef struct {
    int open;
    char wire[1 << 16];
    size_t wire_len;
} GateSend;
static kl_ssize_t gate_send(void *ctx, KlSocketHandle fd, const void *buf, size_t len) {
    GateSend *g = ctx;
    (void)fd;
    if (!g->open) return 0;
    size_t room = sizeof g->wire - g->wire_len;
    size_t n = len < room ? len : room;
    memcpy(g->wire + g->wire_len, buf, n);
    g->wire_len += n;
    return (kl_ssize_t)n;
}
static unsigned gate_caps(const KlEventLoop *loop) { (void)loop; return KL_EVENT_CAP_READINESS; }

/* The default allocator, except that an allocation of exactly g_fail_size bytes fails. */
static size_t g_fail_size;
static void *fail_malloc(void *ctx, size_t size) {
    (void)ctx;
    if (g_fail_size && size == g_fail_size) return NULL;
    return malloc(size);
}
static void *fail_realloc(void *ctx, void *ptr, size_t old, size_t size) {
    (void)ctx; (void)old;
    return realloc(ptr, size);
}
static void fail_free(void *ctx, void *ptr, size_t size) { (void)ctx; (void)size; free(ptr); }

typedef struct {
    GateSend g;
    KlSocketProvider provider;
    KlEventCtx ev;
    KlAllocator alloc;
    KlHttpResponse res;
    KlHttpCompressStream cs;
    int fired;
} Gate;
static const KlSocketOps g_gate_ops = { .send = gate_send, .name = "gate" };
static const KlEventOps g_gate_ev = { .caps = gate_caps };
static void gate_on_writable(void *ctx) { ((Gate *)ctx)->fired++; }

/* A compressed stream begun with capacity `cap`; the head is on the wire, then the gate closes. */
static int gate_begin(Gate *t, size_t cap) {
    memset(t, 0, sizeof *t);
    t->provider = (KlSocketProvider){ .ops = &g_gate_ops, .context = &t->g };
    t->ev.loop.ops = &g_gate_ev;
    t->ev.sockets = &t->provider;
    t->alloc = (KlAllocator){ .malloc = fail_malloc, .realloc = fail_realloc, .free = fail_free };
    if (kl_http_response_init(&t->res, &t->alloc) < 0) return -1;
    t->res.conn_fd = 1;
    t->res.ctx = &t->ev;
    t->res.stream_capacity = cap;
    t->g.open = 1;
    KlCompressConfig cfg = { .factory = bmock_factory };
    if (kl_http_compress_stream_begin(&t->res, &cfg, 200, &t->cs) < 0) return -1;
    t->g.wire_len = 0;
    t->g.open = 0;
    return kl_http_response_stream_on_writable(&t->res, gate_on_writable, t);
}
static void gate_progress(Gate *t) {
    t->g.open = 1;
    (void)kl_drain_flush(&t->res.drain);
    kl_http_response_stream_progress(&t->res);
}
static void gate_free(Gate *t) {
    if (t->cs.comp) (void)kl_http_compress_stream_end(&t->cs);
    kl_http_response_free(&t->res);
}

/* ── Tests ──────────────────────────────────────────────────────────────────────────────────── */

UTEST(compress_bounded, an_accepted_write_is_one_flushed_chunk) {
    Gate t;
    ASSERT_EQ(gate_begin(&t, 4096), 0);
    EXPECT_TRUE(kl_http_compress_stream_bounded(&t.cs));
    t.g.open = 1;
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, "abc", 3), KL_HTTP_STREAM_ACCEPTED);
    EXPECT_EQ(g_m.feeds, 1);
    EXPECT_EQ(g_m.last_flush, KL_COMPRESS_FLUSH_SYNC);     /* flushed: nothing held for later */
    EXPECT_EQ(t.g.wire_len, (size_t)(3 + 7 + 2));           /* "7\r\n" "C:abc|S" "\r\n" */
    EXPECT_EQ(memcmp(t.g.wire, "7\r\nC:abc|S\r\n", t.g.wire_len), 0);
    gate_free(&t);
}

UTEST(compress_bounded, would_block_leaves_the_codec_and_the_output_unchanged) {
    Gate t;
    ASSERT_EQ(gate_begin(&t, 1024), 0);
    static char p[600];
    memset(p, 'p', sizeof p);
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, p, 600), KL_HTTP_STREAM_ACCEPTED);
    size_t pending = kl_http_response_stream_pending(&t.res);
    int feeds = g_m.feeds;
    size_t fed = g_m.fed;
    /* 600 bytes bound to 604, framed 611: 413 left. 404 bytes raw would frame to 411 and fit, but
     * their bound (408) frames to 415: admission goes by the codec's worst case. */
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, p, 404), KL_HTTP_STREAM_WOULD_BLOCK);
    EXPECT_EQ(g_m.feeds, feeds);                           /* the codec was never called */
    EXPECT_EQ(g_m.fed, fed);
    EXPECT_EQ(kl_http_response_stream_pending(&t.res), pending);
    EXPECT_TRUE(t.res.stream_writable_armed);
    EXPECT_FALSE(t.cs.error);
    EXPECT_EQ(t.fired, 0);
    gate_progress(&t);
    EXPECT_EQ(t.fired, 1);                                 /* the response's edge, once */
    gate_progress(&t);
    EXPECT_EQ(t.fired, 1);
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, p, 404), KL_HTTP_STREAM_ACCEPTED);
    EXPECT_FALSE(t.res.stream_writable_armed);
    gate_free(&t);
}

UTEST(compress_bounded, too_large_feeds_nothing_and_does_not_poison) {
    Gate t;
    ASSERT_EQ(gate_begin(&t, 1024), 0);
    static char p[1100];
    memset(p, 'p', sizeof p);
    t.g.open = 1;
    /* Bound 1014 + 4 frames to 1025 bytes with "3f6\r\n" + "\r\n": one past the capacity; the
     * same input raw would fit (1014 + 7). Admission is against the codec's worst case. */
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, p, 1014), KL_HTTP_STREAM_TOO_LARGE);
    EXPECT_EQ(g_m.feeds, 0);
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, p, 1013), KL_HTTP_STREAM_ACCEPTED);
    EXPECT_FALSE(t.cs.error);
    g_m.huge_bound = 1;                                    /* a bound that does not fit a size_t */
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, "x", 1), KL_HTTP_STREAM_TOO_LARGE);
    EXPECT_EQ(g_m.feeds, 1);
    g_m.huge_bound = 0;
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, "x", 1), KL_HTTP_STREAM_ACCEPTED);
    gate_free(&t);
}

UTEST(compress_bounded, a_codec_without_flush_bound_is_refused_and_never_fed) {
    g_no_bound = 1;
    Gate t;
    ASSERT_EQ(gate_begin(&t, 4096), 0);
    g_no_bound = 0;
    EXPECT_FALSE(kl_http_compress_stream_bounded(&t.cs));
    t.g.open = 1;
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, "abc", 3), KL_HTTP_STREAM_ERROR);
    EXPECT_EQ(g_m.feeds, 0);
    EXPECT_EQ(t.g.wire_len, (size_t)0);
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, "abc", 3), KL_HTTP_STREAM_ERROR); /* sticky */
    EXPECT_EQ(kl_http_compress_stream_try_end(&t.cs), KL_HTTP_STREAM_ERROR);
    EXPECT_EQ(g_m.feeds, 0);
    EXPECT_TRUE(g_m.destroyed);                            /* try_end released it */
    EXPECT_TRUE(t.cs.comp == NULL);
    EXPECT_EQ(kl_http_compress_stream_try_end(&t.cs), KL_HTTP_STREAM_CLOSED);
    gate_free(&t);
}

UTEST(compress_bounded, a_codec_past_its_bound_fails_the_stream_before_any_byte_leaves) {
    Gate t;
    ASSERT_EQ(gate_begin(&t, 4096), 0);
    t.g.open = 1;
    g_m.overshoot = 1;
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, "abc", 3), KL_HTTP_STREAM_ERROR);
    EXPECT_EQ(t.g.wire_len, (size_t)0);                    /* the over-long output never went out */
    EXPECT_TRUE(t.cs.error);
    EXPECT_TRUE(t.res.stream_error);                       /* the response failed with it */
    EXPECT_EQ(kl_http_response_stream_try_write(&t.res, "raw", 3), KL_HTTP_STREAM_ERROR);
    g_m.overshoot = 0;
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, "abc", 3), KL_HTTP_STREAM_ERROR);
    gate_free(&t);
}

UTEST(compress_bounded, zero_length_is_accepted_and_feeds_nothing) {
    Gate t;
    ASSERT_EQ(gate_begin(&t, 4096), 0);
    t.g.open = 1;
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, NULL, 0), KL_HTTP_STREAM_ACCEPTED);
    EXPECT_EQ(g_m.feeds, 0);
    EXPECT_EQ(t.g.wire_len, (size_t)0);
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, NULL, 3), KL_HTTP_STREAM_ERROR);
    gate_free(&t);
}

UTEST(compress_bounded, try_end_waits_for_room_then_finishes_and_releases) {
    Gate t;
    ASSERT_EQ(gate_begin(&t, 64), 0);
    static char p[64];
    memset(p, 'p', sizeof p);
    /* 46 bytes bound to 50, framed 50 + "32\r\n" + "\r\n" = 56 of 64: the finish (bound 9, framed
     * 14) no longer fits. */
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, p, 46), KL_HTTP_STREAM_ACCEPTED);
    EXPECT_EQ(kl_http_compress_stream_try_end(&t.cs), KL_HTTP_STREAM_WOULD_BLOCK);
    EXPECT_EQ(g_m.feeds, 1);                               /* the finish was not fed */
    EXPECT_FALSE(g_m.destroyed);
    EXPECT_TRUE(t.res.stream_writable_armed);
    gate_progress(&t);
    EXPECT_EQ(t.fired, 1);
    size_t before = t.g.wire_len;
    EXPECT_EQ(kl_http_compress_stream_try_end(&t.cs), KL_HTTP_STREAM_ACCEPTED);
    EXPECT_EQ(g_m.last_flush, 1);
    EXPECT_TRUE(g_m.destroyed);
    EXPECT_TRUE(t.cs.comp == NULL);
    EXPECT_TRUE(t.cs.stage == NULL);
    EXPECT_EQ(t.g.wire_len - before, (size_t)(3 + 5 + 2 + 5));   /* "5\r\n[END]\r\n" "0\r\n\r\n" */
    EXPECT_EQ(memcmp(t.g.wire + before, "5\r\n[END]\r\n0\r\n\r\n", 15), 0);
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, "x", 1), KL_HTTP_STREAM_CLOSED);
    EXPECT_EQ(kl_http_compress_stream_try_end(&t.cs), KL_HTTP_STREAM_CLOSED);
    gate_free(&t);
}

UTEST(compress_bounded, try_end_reports_a_capacity_below_the_finish_and_changes_nothing) {
    Gate t;
    ASSERT_EQ(gate_begin(&t, 12), 0);                      /* the finish frames to 14 */
    t.g.open = 1;
    EXPECT_EQ(kl_http_compress_stream_try_end(&t.cs), KL_HTTP_STREAM_TOO_LARGE);
    EXPECT_EQ(g_m.feeds, 0);
    EXPECT_FALSE(g_m.destroyed);
    EXPECT_TRUE(t.cs.comp != NULL);                        /* still the caller's to release */
    EXPECT_EQ(kl_http_compress_stream_try_end(&t.cs), KL_HTTP_STREAM_TOO_LARGE);   /* permanent */
    (void)kl_http_compress_stream_end(&t.cs);
    EXPECT_TRUE(g_m.destroyed);
    gate_free(&t);
}

UTEST(compress_bounded, try_end_on_an_ended_response_releases_without_feeding) {
    Gate t;
    ASSERT_EQ(gate_begin(&t, 4096), 0);
    t.g.open = 1;
    EXPECT_EQ(kl_http_response_end_stream(&t.res), 0);
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, "x", 1), KL_HTTP_STREAM_CLOSED);
    EXPECT_EQ(kl_http_compress_stream_try_end(&t.cs), KL_HTTP_STREAM_CLOSED);
    EXPECT_EQ(g_m.feeds, 0);
    EXPECT_TRUE(g_m.destroyed);
    gate_free(&t);
}

UTEST(compress_bounded, a_failed_staging_allocation_feeds_nothing_and_fails_the_stream) {
    Gate t;
    ASSERT_EQ(gate_begin(&t, 4096), 0);
    t.g.open = 1;
    g_fail_size = 2 + 77 + 2;                              /* the stage for a 77-byte write */
    static char p[77];
    memset(p, 'p', sizeof p);
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, p, 77), KL_HTTP_STREAM_ERROR);
    g_fail_size = 0;
    EXPECT_EQ(g_m.feeds, 0);                               /* refused before the codec ran */
    EXPECT_EQ(t.g.wire_len, (size_t)0);
    EXPECT_TRUE(t.res.stream_error);
    kl_http_compress_stream_release(&t.cs);
    EXPECT_TRUE(g_m.destroyed);
    gate_free(&t);
}

UTEST(compress_bounded, a_head_response_takes_writes_without_running_the_codec) {
    Gate t;
    ASSERT_EQ(gate_begin(&t, 64), 0);
    t.res.head_request = 1;
    t.g.open = 1;
    static char big[100000];                               /* far past the capacity */
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, big, sizeof big), KL_HTTP_STREAM_ACCEPTED);
    EXPECT_EQ(g_m.feeds, 0);
    EXPECT_TRUE(t.cs.stage == NULL);                       /* nothing staged for a body never sent */
    EXPECT_EQ(t.g.wire_len, (size_t)0);
    /* A legacy write still feeds the codec (its output is dropped on HEAD); the finish must not
     * then size staging for what the codec owes, nor run the codec at all. */
    EXPECT_EQ(kl_http_compress_stream_write(&t.cs, big, sizeof big), 0);
    int feeds = g_m.feeds;
    EXPECT_EQ(kl_http_compress_stream_try_end(&t.cs), KL_HTTP_STREAM_ACCEPTED);
    EXPECT_EQ(g_m.feeds, feeds);                           /* no finish fed */
    EXPECT_TRUE(t.res.stream_ended);
    EXPECT_EQ(t.g.wire_len, (size_t)0);
    EXPECT_TRUE(g_m.destroyed);
    gate_free(&t);
}

UTEST(compress_bounded, release_writes_nothing_and_is_idempotent) {
    Gate t;
    ASSERT_EQ(gate_begin(&t, 1024), 0);
    static char p[600];
    memset(p, 'p', sizeof p);
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, p, 600), KL_HTTP_STREAM_ACCEPTED);
    EXPECT_EQ(kl_http_compress_stream_try_end(&t.cs), KL_HTTP_STREAM_ACCEPTED);
    gate_free(&t);

    /* A full stream abandoned (the on_cancel path): the finish is never written. */
    ASSERT_EQ(gate_begin(&t, 1024), 0);
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, p, 600), KL_HTTP_STREAM_ACCEPTED);
    EXPECT_EQ(kl_http_compress_stream_try_end(&t.cs), KL_HTTP_STREAM_ACCEPTED);
    gate_free(&t);
    ASSERT_EQ(gate_begin(&t, 620), 0);
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, p, 600), KL_HTTP_STREAM_ACCEPTED);
    EXPECT_EQ(kl_http_compress_stream_try_end(&t.cs), KL_HTTP_STREAM_WOULD_BLOCK);
    size_t pending = kl_http_response_stream_pending(&t.res);
    int feeds = g_m.feeds;
    kl_http_compress_stream_release(&t.cs);
    EXPECT_TRUE(g_m.destroyed);
    EXPECT_EQ(g_m.feeds, feeds);                           /* no finish fed */
    EXPECT_EQ(kl_http_response_stream_pending(&t.res), pending);   /* nothing written */
    EXPECT_TRUE(t.cs.comp == NULL);
    EXPECT_TRUE(t.cs.stage == NULL);
    kl_http_compress_stream_release(&t.cs);                /* idempotent */
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, "x", 1), KL_HTTP_STREAM_CLOSED);
    kl_http_compress_stream_release(NULL);
    gate_free(&t);
}

UTEST(compress_bounded, end_releases_a_bounded_stream_in_any_state) {
    Gate t;
    ASSERT_EQ(gate_begin(&t, 1024), 0);
    static char p[600];
    memset(p, 'p', sizeof p);
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, p, 600), KL_HTTP_STREAM_ACCEPTED);
    EXPECT_TRUE(t.cs.stage != NULL);
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, p, 600), KL_HTTP_STREAM_WOULD_BLOCK);
    /* The cancel path: a full stream released through the legacy end (ASan: nothing leaks). */
    (void)kl_http_compress_stream_end(&t.cs);
    EXPECT_TRUE(g_m.destroyed);
    EXPECT_TRUE(t.cs.comp == NULL);
    EXPECT_TRUE(t.cs.stage == NULL);
    EXPECT_EQ(t.cs.stage_cap, (size_t)0);
    gate_free(&t);
}

UTEST(compress_bounded, the_staging_buffer_grows_to_the_largest_bound_only) {
    Gate t;
    ASSERT_EQ(gate_begin(&t, 4096), 0);
    static char p[2000];
    memset(p, 'p', sizeof p);
    t.g.open = 1;
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, p, 100), KL_HTTP_STREAM_ACCEPTED);
    EXPECT_EQ(t.cs.stage_cap, (size_t)104);
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, p, 2000), KL_HTTP_STREAM_ACCEPTED);
    EXPECT_EQ(t.cs.stage_cap, (size_t)2004);
    EXPECT_EQ(kl_http_compress_stream_try_write(&t.cs, p, 10), KL_HTTP_STREAM_ACCEPTED);
    EXPECT_EQ(t.cs.stage_cap, (size_t)2004);              /* reused, not regrown */
    EXPECT_TRUE(t.cs.stage_cap <= kl_http_response_stream_capacity(&t.res));
    gate_free(&t);
}

UTEST(compress_bounded, the_legacy_write_is_unchanged) {
    Gate t;
    ASSERT_EQ(gate_begin(&t, 4096), 0);
    t.g.open = 1;
    EXPECT_EQ(kl_http_compress_stream_write(&t.cs, "abc", 3), 0);
    EXPECT_EQ(g_m.last_flush, 0);                          /* never flushed per write */
    EXPECT_EQ(memcmp(t.g.wire, "2\r\nC:\r\n3\r\nabc\r\n", 15), 0);
    EXPECT_EQ(kl_http_compress_stream_end(&t.cs), 0);
    EXPECT_EQ(g_m.last_flush, 1);
    gate_free(&t);
}

UTEST(compress_bounded, null_and_closed) {
    EXPECT_EQ(kl_http_compress_stream_try_write(NULL, "x", 1), KL_HTTP_STREAM_ERROR);
    EXPECT_EQ(kl_http_compress_stream_try_end(NULL), KL_HTTP_STREAM_ERROR);
    EXPECT_FALSE(kl_http_compress_stream_bounded(NULL));
    KlHttpCompressStream cs;
    memset(&cs, 0, sizeof cs);
    EXPECT_EQ(kl_http_compress_stream_try_write(&cs, "x", 1), KL_HTTP_STREAM_CLOSED);
    EXPECT_EQ(kl_http_compress_stream_try_end(&cs), KL_HTTP_STREAM_CLOSED);
}

UTEST_MAIN()
