/*
 * test_stream_writable.c: the KlStream writable-again edge (kl_stream_on_writable), with mock
 * readiness-writer and completion-submit hooks (no live sockets).
 *
 * The contract under test, one rule per case:
 *   - only a WOULD_BLOCK arms it; bytes draining alone never fire it;
 *   - an ACCEPTED write disarms it;
 *   - it fires once per arming, from kl_stream_flush / kl_stream_on_write_complete only, never from
 *     inside kl_stream_write;
 *   - "full" is the stream's TOTAL pending count: in-flight bytes a copying backend has already
 *     consumed from the queue still hold it back;
 *   - a terminal write failure wakes an armed producer once, and its retry reports ERROR; once
 *     whether the callback retries or not;
 *   - it never fires once a close has begun;
 *   - the callback may write, and may begin a close whose detachment waits for it to return;
 *   - a refusal seen before the callback was installed is remembered: installed after the queue has
 *     drained, it fires once from the stream's loop (a deferred check), never inside the install, with
 *     no further I/O; a replacement, a removal, an accepted write, a close or a free before that
 *     check cancels it.
 * The last case drives a producer purely by the edge over a real loopback socket (the readiness
 * writer path, so it runs on every engine). The completion counterpart over a named pipe lives in
 * test_pipe_stream.c.
 */
#include "utest.h"
#include <keel/allocator.h>
#include <keel/stream.h>
#include <keel/stream_detail.h>
#include <keel/sockaddr.h>
#include <keel/handle.h>
#include <keel/event_ctx.h>
#include <keel/timer.h>           /* kl_timer_fire: counts the deferred checks still pending */
#include "../src/stream_write.h"  /* kl_stream_set_owner_hold (internal adapter hook) */
#include "../src/socket.h"        /* kl_sockdef_*: the default socket seam (live section) */
#include "net_compat.h"           /* AF_INET / SOCK_STREAM */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ── Mocks ──────────────────────────────────────────────────────────────────────────────────── */

/* Readiness writer. mode 0 = accept all, 2 = would-block, 3 = error. mode 1 = accept at most
 * `budget` more bytes in total, then would-block (a flush loops until the writer stalls). */
typedef struct { size_t len; int mode; size_t budget; } RW;
static kl_ssize_t rw_write(const char *data, size_t len, void *ctx) {
    RW *w = ctx; (void)data;
    if (w->mode == 3) return -1;
    if (w->mode == 2) return 0;
    size_t n = len;
    if (w->mode == 1) { if (n > w->budget) n = w->budget; w->budget -= n; }
    w->len += n;
    return (kl_ssize_t)n;
}

/* Completion submitter: records the batch; the test delivers the completion. */
typedef struct { KlStream *s; int submits; size_t last_len; } CS;
static int cs_submit(void *ctx, const char *data, size_t len) {
    CS *c = ctx; (void)data;
    c->submits++; c->last_len = len;
    return 0;
}

/* The producer: counts edges, records whether one arrived inside kl_stream_write, and can act. */
typedef struct {
    KlStream *s;
    int fired;
    int in_write;          /* set by the test around its own kl_stream_write calls */
    int fired_in_write;
    int action;            /* 0 none, 1 write `wlen` bytes, 2 close_begin, 3 cancel,
                            * 4 stall the readiness writer `rw` then write `wlen` bytes,
                            * 5 stall it, write `wlen` bytes, then `wlen` more (refused again) */
    RW *rw;
    size_t wlen;
    KlStreamWriteStatus wst;
    int closed_during_cb;  /* on_close seen while the callback was still running */
} Prod;
static int g_closes;
static void on_close(void *ctx) { (void)ctx; g_closes++; }
static void prod_writable(void *ctx) {
    Prod *p = ctx;
    p->fired++;
    if (p->in_write) p->fired_in_write++;
    static const char fill[4096];
    if (p->action == 1) p->wst = kl_stream_write(p->s, fill, p->wlen);
    if (p->action == 2) (void)kl_stream_close_begin(p->s);
    if (p->action == 3) (void)kl_stream_cancel(p->s);
    if (p->action == 4) { p->rw->mode = 2; p->wst = kl_stream_write(p->s, fill, p->wlen); }
    if (p->action == 5) {                  /* stall the writer, fill the queue, be refused again */
        p->rw->mode = 2;
        (void)kl_stream_write(p->s, fill, p->wlen);
        p->wst = kl_stream_write(p->s, fill, p->wlen);
    }
    if (g_closes) p->closed_during_cb = 1;
}
static KlStreamWriteStatus pw(Prod *p, size_t len) {
    static const char fill[4096];
    p->in_write = 1;
    KlStreamWriteStatus r = kl_stream_write(p->s, fill, len);
    p->in_write = 0;
    return r;
}

static KlAllocator g_alloc;
static char g_rbuf[64];

static void readiness(KlStream *s, RW *w, Prod *p, size_t cap) {
    g_alloc = kl_allocator_default();
    memset(w, 0, sizeof *w); memset(p, 0, sizeof *p); p->s = s;
    g_closes = 0;
    kl_stream_init(s, g_rbuf, sizeof g_rbuf);
    kl_stream_write_init(s, &g_alloc, cap);
    kl_stream_set_writer(s, rw_write, w);
    kl_stream_on_writable(s, prod_writable, p);
}
static void completion(KlStream *s, CS *c, Prod *p, size_t cap, int copying) {
    g_alloc = kl_allocator_default();
    memset(c, 0, sizeof *c); c->s = s;
    memset(p, 0, sizeof *p); p->s = s;
    g_closes = 0;
    kl_stream_init(s, g_rbuf, sizeof g_rbuf);
    kl_stream_write_init(s, &g_alloc, cap);
    kl_stream_set_submit(s, cs_submit, c, copying);
    kl_stream_on_writable(s, prod_writable, p);
}

/* ── Arming ─────────────────────────────────────────────────────────────────────────────────── */

UTEST(stream_writable, requires_write_init) {
    KlStream s; kl_stream_init(&s, g_rbuf, sizeof g_rbuf);
    ASSERT_EQ(kl_stream_on_writable(&s, prod_writable, NULL), -1);
    ASSERT_EQ(kl_stream_on_writable(NULL, prod_writable, NULL), -1);
}

UTEST(stream_writable, draining_alone_never_fires) {
    KlStream s; RW w; Prod p; readiness(&s, &w, &p, 64);
    w.mode = 2;
    ASSERT_EQ((int)pw(&p, 40), (int)KL_STREAM_ACCEPTED);   /* queued, never blocked */
    w.mode = 0;
    ASSERT_EQ(kl_stream_flush(&s), 0);                    /* fully drained ... */
    ASSERT_EQ(p.fired, 0);                                /* ... but nobody was blocked */
    ASSERT_EQ(kl_stream_write_free(&s), 0);                /* no send left in flight */
}

UTEST(stream_writable, readiness_fires_once_when_pending_drops_below_capacity) {
    KlStream s; RW w; Prod p; readiness(&s, &w, &p, 64);
    w.mode = 2;
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);   /* queue exactly full */
    ASSERT_EQ((int)pw(&p, 1), (int)KL_STREAM_WOULD_BLOCK); /* arms */
    ASSERT_EQ(kl_stream_flush(&s), 1);                    /* no progress: still full */
    ASSERT_EQ(p.fired, 0);
    w.mode = 1; w.budget = 10;
    ASSERT_EQ(kl_stream_flush(&s), 1);                    /* 54 pending < 64: the edge */
    ASSERT_EQ(p.fired, 1);
    w.budget = 10;
    ASSERT_EQ(kl_stream_flush(&s), 1);                    /* more progress: no second edge */
    w.mode = 0;
    ASSERT_EQ(kl_stream_flush(&s), 0);
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ(p.fired_in_write, 0);
    ASSERT_EQ(kl_stream_write_free(&s), 0);                /* no send left in flight */
}

UTEST(stream_writable, accepted_write_disarms) {
    KlStream s; RW w; Prod p; readiness(&s, &w, &p, 64);
    w.mode = 2;
    ASSERT_EQ((int)pw(&p, 60), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 10), (int)KL_STREAM_WOULD_BLOCK);  /* armed */
    ASSERT_EQ((int)pw(&p, 4), (int)KL_STREAM_ACCEPTED);      /* the producer moved on: disarmed */
    w.mode = 0;
    ASSERT_EQ(kl_stream_flush(&s), 0);
    ASSERT_EQ(p.fired, 0);
    ASSERT_EQ(kl_stream_write_free(&s), 0);                /* no send left in flight */
}

UTEST(stream_writable, rearms_on_each_would_block) {
    KlStream s; RW w; Prod p; readiness(&s, &w, &p, 64);
    w.mode = 2;
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 30), (int)KL_STREAM_WOULD_BLOCK);
    w.mode = 1; w.budget = 10;
    ASSERT_EQ(kl_stream_flush(&s), 1);                       /* 54 pending: fires ... */
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ((int)pw(&p, 30), (int)KL_STREAM_WOULD_BLOCK);  /* ... but 30 still does not fit */
    w.budget = 10;
    ASSERT_EQ(kl_stream_flush(&s), 1);                       /* re-armed: the next progress fires */
    ASSERT_EQ(p.fired, 2);
    w.mode = 0;
    ASSERT_EQ(kl_stream_flush(&s), 0);
    ASSERT_EQ((int)pw(&p, 30), (int)KL_STREAM_ACCEPTED);     /* the retry now fits */
    ASSERT_EQ(p.fired, 2);
    ASSERT_EQ(kl_stream_write_free(&s), 0);                /* no send left in flight */
}

UTEST(stream_writable, removing_the_callback_disarms) {
    KlStream s; RW w; Prod p; readiness(&s, &w, &p, 64);
    w.mode = 2;
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 1), (int)KL_STREAM_WOULD_BLOCK);
    ASSERT_EQ(kl_stream_on_writable(&s, NULL, NULL), 0);
    ASSERT_EQ(kl_stream_on_writable(&s, prod_writable, &p), 0);  /* reinstalled: not re-armed */
    w.mode = 0;
    ASSERT_EQ(kl_stream_flush(&s), 0);
    ASSERT_EQ(p.fired, 0);
    ASSERT_EQ(kl_stream_write_free(&s), 0);                /* no send left in flight */
}

/* ── Completion: in-flight bytes count ──────────────────────────────────────────────────────── */

UTEST(stream_writable, copying_in_flight_bytes_hold_the_edge) {
    KlStream s; CS c; Prod p; completion(&s, &c, &p, 64, /*copying=*/1);
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);    /* submitted; queue consumed at submit */
    ASSERT_EQ(c.submits, 1);
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);    /* queued behind the in-flight send */
    ASSERT_EQ((int)kl_stream_write_pending(&s), 128);
    ASSERT_EQ((int)pw(&p, 1), (int)KL_STREAM_WOULD_BLOCK);  /* armed */

    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);      /* 64 retired, the next 64 submitted */
    ASSERT_EQ(c.submits, 2);
    ASSERT_EQ((int)kl_stream_write_pending(&s), 64);        /* queue EMPTY, but 64 still in flight */
    ASSERT_EQ(p.fired, 0);                                  /* not below capacity: no edge */

    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);
    ASSERT_EQ((int)kl_stream_write_pending(&s), 0);
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ(p.fired_in_write, 0);
    ASSERT_EQ(kl_stream_write_free(&s), 0);                /* no send left in flight */
}

UTEST(stream_writable, referencing_backend_fires_on_completion) {
    KlStream s; CS c; Prod p; completion(&s, &c, &p, 64, /*copying=*/0);
    ASSERT_EQ((int)pw(&p, 40), (int)KL_STREAM_ACCEPTED);    /* in flight, still in the queue */
    ASSERT_EQ((int)pw(&p, 24), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 1), (int)KL_STREAM_WOULD_BLOCK);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);      /* 40 consumed; 24 submitted */
    ASSERT_EQ((int)kl_stream_write_pending(&s), 24);
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);
    ASSERT_EQ(p.fired, 1);                                  /* once per arming */
    ASSERT_EQ(kl_stream_write_free(&s), 0);                /* no send left in flight */
}

/* ── Never from inside kl_stream_write ──────────────────────────────────────────────────────── */

UTEST(stream_writable, write_never_calls_back_the_producer) {
    KlStream s; CS c; Prod p; completion(&s, &c, &p, 64, /*copying=*/1);
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 8), (int)KL_STREAM_WOULD_BLOCK);  /* armed */
    ASSERT_EQ((int)pw(&p, 8), (int)KL_STREAM_WOULD_BLOCK);  /* writes while armed: no callback */
    ASSERT_EQ(p.fired, 0);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);      /* 64 still in flight: no edge yet */
    ASSERT_EQ(p.fired, 0);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);      /* the edge comes from the completion */
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ((int)pw(&p, 8), (int)KL_STREAM_ACCEPTED);    /* the accepted retry submits ... */
    ASSERT_EQ(p.fired, 1);                                  /* ... without calling back */
    ASSERT_EQ(p.fired_in_write, 0);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ(kl_stream_write_free(&s), 0);                /* no send left in flight */
}

UTEST(stream_writable, completion_accepted_write_while_armed_disarms) {
    KlStream s; CS c; Prod p; completion(&s, &c, &p, 64, /*copying=*/1);
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 60), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 8), (int)KL_STREAM_WOULD_BLOCK);  /* armed */
    ASSERT_EQ((int)pw(&p, 4), (int)KL_STREAM_ACCEPTED);     /* fits: the producer moved on */
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);
    ASSERT_EQ((int)kl_stream_write_pending(&s), 0);
    ASSERT_EQ(p.fired, 0);
    ASSERT_EQ(kl_stream_write_free(&s), 0);                /* no send left in flight */
}

/* ── The callback's own actions ─────────────────────────────────────────────────────────────── */

UTEST(stream_writable, callback_may_write_and_flush_reports_the_refill) {
    KlStream s; RW w; Prod p; readiness(&s, &w, &p, 64);
    w.mode = 2;
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 1), (int)KL_STREAM_WOULD_BLOCK);
    p.action = 1; p.wlen = 32;
    w.mode = 0;
    int r = kl_stream_flush(&s);                           /* drains, fires, the callback writes */
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ((int)p.wst, (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ(r, 0);                                       /* the refill went out inline */
    ASSERT_EQ((int)kl_stream_write_pending(&s), 0);
    ASSERT_EQ((int)w.len, 96);
    ASSERT_EQ(kl_stream_write_free(&s), 0);                /* no send left in flight */
}

UTEST(stream_writable, callback_refill_that_buffers_keeps_flush_pending) {
    KlStream s; RW w; Prod p; readiness(&s, &w, &p, 64);
    w.mode = 2;
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 1), (int)KL_STREAM_WOULD_BLOCK);
    p.action = 4; p.wlen = 32; p.rw = &w;                  /* the refill finds the writer stalled */
    w.mode = 0;
    ASSERT_EQ(kl_stream_flush(&s), 1);                     /* drained, then refilled: still pending */
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ((int)p.wst, (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)kl_stream_write_pending(&s), 32);
    w.mode = 0;
    ASSERT_EQ(kl_stream_flush(&s), 0);
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ(kl_stream_write_free(&s), 0);                /* no send left in flight */
}

UTEST(stream_writable, completion_callback_write_is_submitted) {
    KlStream s; CS c; Prod p; completion(&s, &c, &p, 64, /*copying=*/1);
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 1), (int)KL_STREAM_WOULD_BLOCK);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);
    p.action = 1; p.wlen = 16;
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);      /* fires; the callback writes 16 */
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ((int)p.wst, (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ(c.submits, 3);                               /* ... and it went straight out */
    ASSERT_EQ((int)c.last_len, 16);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ(kl_stream_write_free(&s), 0);                /* no send left in flight */
}

/* ── Failure and close ──────────────────────────────────────────────────────────────────────── */

UTEST(stream_writable, terminal_failure_wakes_an_armed_producer_once) {
    KlStream s; CS c; Prod p; completion(&s, &c, &p, 64, /*copying=*/1);
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 1), (int)KL_STREAM_WOULD_BLOCK);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 0), -1);     /* the send failed */
    ASSERT_EQ(p.fired, 1);                                  /* woken ... */
    ASSERT_EQ((int)pw(&p, 1), (int)KL_STREAM_ERROR);        /* ... and the retry reports it */
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ(kl_stream_write_free(&s), 0);                /* no send left in flight */
}

UTEST(stream_writable, readiness_failure_wakes_an_armed_producer_once) {
    KlStream s; RW w; Prod p; readiness(&s, &w, &p, 64);
    w.mode = 2;
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 1), (int)KL_STREAM_WOULD_BLOCK);
    w.mode = 3;
    ASSERT_EQ(kl_stream_flush(&s), -1);
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ((int)pw(&p, 1), (int)KL_STREAM_ERROR);
    ASSERT_EQ(kl_stream_flush(&s), -1);
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ(kl_stream_write_free(&s), 0);                /* no send left in flight */
}

/* "Wake once" is the stream's property, independent of what the consumer does with the wakeup. */

UTEST(stream_writable, failure_wake_without_retry_is_not_repeated) {
    KlStream s; CS c; Prod p; completion(&s, &c, &p, 64, /*copying=*/1);
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 1), (int)KL_STREAM_WOULD_BLOCK);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 0), -1);     /* terminal; the callback does nothing */
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 0), -1);     /* late / duplicate completions ... */
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), -1);
    ASSERT_EQ(kl_stream_flush(&s), -1);                    /* ... and a stray flush */
    ASSERT_EQ(p.fired, 1);                                 /* never a second wakeup */
    ASSERT_EQ(kl_stream_write_free(&s), 0);                /* no send left in flight */
}

UTEST(stream_writable, failure_wake_with_retry_reports_error_once) {
    KlStream s; CS c; Prod p; completion(&s, &c, &p, 64, /*copying=*/1);
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 1), (int)KL_STREAM_WOULD_BLOCK);
    p.action = 1; p.wlen = 1;                              /* the callback retries */
    ASSERT_EQ(kl_stream_on_write_complete(&s, 0), -1);
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ((int)p.wst, (int)KL_STREAM_ERROR);           /* the retry is the source of truth */
    ASSERT_EQ(kl_stream_on_write_complete(&s, 0), -1);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), -1);
    ASSERT_EQ(p.fired, 1);                                 /* ERROR did not re-arm it */
    ASSERT_EQ(kl_stream_write_free(&s), 0);                /* no send left in flight */
}

UTEST(stream_writable, readiness_failure_wake_with_and_without_retry_once) {
    for (int retry = 0; retry <= 1; retry++) {
        KlStream s; RW w; Prod p; readiness(&s, &w, &p, 64);
        w.mode = 2;
        ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
        ASSERT_EQ((int)pw(&p, 1), (int)KL_STREAM_WOULD_BLOCK);
        if (retry) { p.action = 1; p.wlen = 1; }
        w.mode = 3;
        ASSERT_EQ(kl_stream_flush(&s), -1);
        ASSERT_EQ(p.fired, 1);
        if (retry) ASSERT_EQ((int)p.wst, (int)KL_STREAM_ERROR);
        ASSERT_EQ(kl_stream_flush(&s), -1);
        ASSERT_EQ(kl_stream_flush(&s), -1);
        ASSERT_EQ(p.fired, 1);
        ASSERT_EQ(kl_stream_write_free(&s), 0);                /* no send left in flight */
    }
}

UTEST(stream_writable, failure_without_a_blocked_producer_is_silent) {
    KlStream s; CS c; Prod p; completion(&s, &c, &p, 64, /*copying=*/1);
    ASSERT_EQ((int)pw(&p, 10), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 0), -1);
    ASSERT_EQ(p.fired, 0);
    ASSERT_EQ(kl_stream_write_free(&s), 0);                /* no send left in flight */
}

UTEST(stream_writable, never_fires_once_closing) {
    KlStream s; CS c; Prod p; completion(&s, &c, &p, 64, /*copying=*/1);
    ASSERT_EQ(kl_stream_close_init(&s, on_close, NULL), 0);
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 1), (int)KL_STREAM_WOULD_BLOCK);
    ASSERT_EQ(kl_stream_close_begin(&s), 0);               /* graceful: drains the queue */
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);
    ASSERT_EQ(g_closes, 1);                                /* on_close is the terminal signal ... */
    ASSERT_EQ(p.fired, 0);                                 /* ... not the writable edge */
    ASSERT_EQ(kl_stream_write_free(&s), 0);                /* no send left in flight */
}

UTEST(stream_writable, close_from_callback_detaches_after_it_returns) {
    KlStream s; CS c; Prod p; completion(&s, &c, &p, 64, /*copying=*/1);
    ASSERT_EQ(kl_stream_close_init(&s, on_close, NULL), 0);
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 1), (int)KL_STREAM_WOULD_BLOCK);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);
    p.action = 2;                                          /* graceful close from the callback */
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);      /* nothing pending: fully retired */
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ(p.closed_during_cb, 0);                      /* deferred past the callback ... */
    ASSERT_EQ(g_closes, 1);                                /* ... then detached, exactly once */
    ASSERT_EQ(kl_stream_is_detached(&s), 1);
    ASSERT_EQ(kl_stream_write_free(&s), 0);                /* no send left in flight */
}

UTEST(stream_writable, cancel_from_readiness_callback_detaches_after_it_returns) {
    KlStream s; RW w; Prod p; readiness(&s, &w, &p, 64);
    ASSERT_EQ(kl_stream_close_init(&s, on_close, NULL), 0);
    w.mode = 2;
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 1), (int)KL_STREAM_WOULD_BLOCK);
    p.action = 3;                                          /* abortive close from the callback */
    w.mode = 1; w.budget = 10;
    ASSERT_EQ(kl_stream_flush(&s), 1);                     /* 54 still queued: flush says pending */
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ(p.closed_during_cb, 0);
    ASSERT_EQ(g_closes, 1);                                /* abortive: the queue does not hold it */
    ASSERT_EQ(kl_stream_write_free(&s), 0);                /* no send left in flight */
}

/* ── Late install: a refusal before the callback existed is remembered ─────────────────────────── */

static KlEventCtx g_ev;
static KlAllocator g_ev_alloc;
static int ev_init(void) { g_ev_alloc = kl_allocator_default(); return kl_event_ctx_init(&g_ev, &g_ev_alloc); }

/* As readiness()/completion(), on a real loop, with NO callback installed yet. */
static void late_readiness(KlStream *s, RW *w, Prod *p, size_t cap) {
    g_alloc = kl_allocator_default();
    memset(w, 0, sizeof *w); memset(p, 0, sizeof *p); p->s = s;
    g_closes = 0;
    kl_stream_init(s, g_rbuf, sizeof g_rbuf);
    s->ctx = &g_ev;
    kl_stream_write_init(s, &g_alloc, cap);
    kl_stream_set_writer(s, rw_write, w);
    kl_stream_close_init(s, on_close, NULL);
}
static void late_completion(KlStream *s, CS *c, Prod *p, size_t cap) {
    g_alloc = kl_allocator_default();
    memset(c, 0, sizeof *c); c->s = s;
    memset(p, 0, sizeof *p); p->s = s;
    g_closes = 0;
    kl_stream_init(s, g_rbuf, sizeof g_rbuf);
    s->ctx = &g_ev;
    kl_stream_write_init(s, &g_alloc, cap);
    kl_stream_set_submit(s, cs_submit, c, /*copying=*/1);
    kl_stream_close_init(s, on_close, NULL);
}
/* Refused with no callback, then the queue drains completely: the last progress point passes with
 * nobody to tell. */
static void refuse_then_drain(KlStream *s, RW *w, Prod *p) {
    w->mode = 2;
    (void)pw(p, 64);                                       /* queue exactly full */
    (void)pw(p, 1);                                        /* WOULD_BLOCK, nobody installed */
    w->mode = 0;
    (void)kl_stream_flush(s);                              /* fully drained */
}

UTEST(stream_writable, late_install_after_full_drain_fires_once_from_the_loop) {
    ASSERT_EQ(ev_init(), 0);
    KlStream s; RW w; Prod p; late_readiness(&s, &w, &p, 64);
    w.mode = 2;
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 1), (int)KL_STREAM_WOULD_BLOCK);
    w.mode = 0;
    ASSERT_EQ(kl_stream_flush(&s), 0);
    ASSERT_EQ((int)kl_stream_write_pending(&s), 0);         /* nothing left: no progress point to come */
    size_t wrote = w.len;
    ASSERT_EQ(kl_stream_on_writable(&s, prod_writable, &p), 0);
    ASSERT_EQ(p.fired, 0);                                  /* never inside the install */
    for (int i = 0; i < 4; i++) ASSERT_GE(kl_event_ctx_run(&g_ev, 8, 0), 0);
    ASSERT_EQ(p.fired, 1);                                  /* exactly once, from the loop */
    ASSERT_EQ(w.len, wrote);                                /* no further transport I/O needed */
    ASSERT_EQ((int)pw(&p, 1), (int)KL_STREAM_ACCEPTED);     /* the retry fits */
    ASSERT_EQ(kl_stream_write_free(&s), 0);
    kl_event_ctx_free(&g_ev);
}

UTEST(stream_writable, late_install_completion_fires_once_without_another_submit) {
    ASSERT_EQ(ev_init(), 0);
    KlStream s; CS c; Prod p; late_completion(&s, &c, &p, 64);
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);    /* submitted; in flight */
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);    /* queued behind it */
    ASSERT_EQ((int)pw(&p, 1), (int)KL_STREAM_WOULD_BLOCK);  /* nobody installed */
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);       /* pumps the second batch */
    ASSERT_EQ(c.submits, 2);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);       /* the last progress point */
    ASSERT_EQ((int)kl_stream_write_pending(&s), 0);
    ASSERT_EQ(kl_stream_on_writable(&s, prod_writable, &p), 0);
    ASSERT_EQ(p.fired, 0);
    ASSERT_EQ(kl_timer_fire(&g_ev), 1);                     /* the single deferred check */
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ(c.submits, 2);                                /* no further I/O */
    ASSERT_EQ(kl_timer_fire(&g_ev), 0);
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ(kl_stream_write_free(&s), 0);
    kl_event_ctx_free(&g_ev);
}

UTEST(stream_writable, late_install_while_still_full_waits_for_progress) {
    ASSERT_EQ(ev_init(), 0);
    KlStream s; RW w; Prod p; late_readiness(&s, &w, &p, 64);
    w.mode = 2;
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 1), (int)KL_STREAM_WOULD_BLOCK);
    ASSERT_EQ(kl_stream_on_writable(&s, prod_writable, &p), 0);   /* bytes pending: progress will come */
    ASSERT_EQ(kl_timer_fire(&g_ev), 0);                     /* nothing deferred */
    ASSERT_EQ(p.fired, 0);
    w.mode = 0;
    ASSERT_EQ(kl_stream_flush(&s), 0);
    ASSERT_EQ(p.fired, 1);                                  /* the ordinary progress-point edge */
    ASSERT_EQ(kl_timer_fire(&g_ev), 0);
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ(kl_stream_write_free(&s), 0);
    kl_event_ctx_free(&g_ev);
}

UTEST(stream_writable, late_install_after_terminal_failure_fires_and_retry_errors) {
    ASSERT_EQ(ev_init(), 0);
    KlStream s; RW w; Prod p; late_readiness(&s, &w, &p, 64);
    w.mode = 2;
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 1), (int)KL_STREAM_WOULD_BLOCK);
    w.mode = 3;
    ASSERT_EQ(kl_stream_flush(&s), -1);                     /* the write side failed, nobody told */
    p.action = 1; p.wlen = 1;
    ASSERT_EQ(kl_stream_on_writable(&s, prod_writable, &p), 0);
    ASSERT_EQ(p.fired, 0);
    ASSERT_EQ(kl_timer_fire(&g_ev), 1);
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ((int)p.wst, (int)KL_STREAM_ERROR);            /* the retry tells it apart */
    ASSERT_EQ(kl_stream_write_free(&s), 0);
    kl_event_ctx_free(&g_ev);
}

UTEST(stream_writable, late_install_replaced_before_the_check_fires_only_the_new_fn) {
    ASSERT_EQ(ev_init(), 0);
    KlStream s; RW w; Prod p; late_readiness(&s, &w, &p, 64);
    refuse_then_drain(&s, &w, &p);
    Prod q; memset(&q, 0, sizeof q); q.s = &s;
    ASSERT_EQ(kl_stream_on_writable(&s, prod_writable, &p), 0);
    ASSERT_EQ(kl_stream_on_writable(&s, prod_writable, &q), 0);   /* replaced before the loop ran */
    ASSERT_EQ(kl_timer_fire(&g_ev), 1);                     /* the old check was cancelled */
    ASSERT_EQ(p.fired, 0);
    ASSERT_EQ(q.fired, 1);
    ASSERT_EQ(kl_stream_write_free(&s), 0);
    kl_event_ctx_free(&g_ev);
}

UTEST(stream_writable, late_install_removed_before_the_check_fires_nothing) {
    ASSERT_EQ(ev_init(), 0);
    KlStream s; RW w; Prod p; late_readiness(&s, &w, &p, 64);
    refuse_then_drain(&s, &w, &p);
    ASSERT_EQ(kl_stream_on_writable(&s, prod_writable, &p), 0);
    ASSERT_EQ(kl_stream_on_writable(&s, NULL, NULL), 0);
    ASSERT_EQ(kl_timer_fire(&g_ev), 0);                     /* cancelled, not merely ignored */
    ASSERT_EQ(p.fired, 0);
    ASSERT_EQ(kl_stream_on_writable(&s, prod_writable, &p), 0);   /* removal disarmed the refusal */
    ASSERT_EQ(kl_timer_fire(&g_ev), 0);
    ASSERT_EQ(p.fired, 0);
    ASSERT_EQ(kl_stream_write_free(&s), 0);
    kl_event_ctx_free(&g_ev);
}

UTEST(stream_writable, late_install_accepted_write_before_the_check_disarms) {
    ASSERT_EQ(ev_init(), 0);
    KlStream s; RW w; Prod p; late_readiness(&s, &w, &p, 64);
    refuse_then_drain(&s, &w, &p);
    ASSERT_EQ(kl_stream_on_writable(&s, prod_writable, &p), 0);
    ASSERT_EQ((int)pw(&p, 1), (int)KL_STREAM_ACCEPTED);     /* the producer moved on by itself */
    ASSERT_EQ(kl_timer_fire(&g_ev), 0);
    ASSERT_EQ(p.fired, 0);
    ASSERT_EQ(kl_stream_write_free(&s), 0);
    kl_event_ctx_free(&g_ev);
}

/* Close begun before the check: on_close is the terminal signal, and the owner may free the stream
 * right after it (heap-allocated, so ASan catches a check that still ran). */
UTEST(stream_writable, late_install_close_before_the_check_never_fires) {
    ASSERT_EQ(ev_init(), 0);
    KlStream *s = malloc(sizeof *s); ASSERT_TRUE(s != NULL);
    RW w; Prod p; late_readiness(s, &w, &p, 64);
    refuse_then_drain(s, &w, &p);
    ASSERT_EQ(kl_stream_on_writable(s, prod_writable, &p), 0);
    ASSERT_EQ(kl_stream_close_begin(s), 0);
    ASSERT_EQ(g_closes, 1);                                 /* nothing pending: detached at once */
    ASSERT_EQ(kl_timer_next_timeout(&g_ev, 1000), 1000);   /* the close itself cancelled the check */
    ASSERT_EQ(kl_stream_write_free(s), 0);
    free(s);
    ASSERT_EQ(kl_timer_fire(&g_ev), 0);
    ASSERT_EQ(p.fired, 0);
    kl_event_ctx_free(&g_ev);
}

UTEST(stream_writable, late_install_close_from_completion_before_the_check_never_fires) {
    ASSERT_EQ(ev_init(), 0);
    KlStream s; CS c; Prod p; late_completion(&s, &c, &p, 64);
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 1), (int)KL_STREAM_WOULD_BLOCK);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);
    ASSERT_EQ(kl_stream_on_writable(&s, prod_writable, &p), 0);
    ASSERT_EQ(kl_stream_cancel(&s), 0);                     /* abortive; nothing in flight */
    ASSERT_EQ(g_closes, 1);
    ASSERT_EQ(kl_timer_fire(&g_ev), 0);
    ASSERT_EQ(p.fired, 0);
    ASSERT_EQ(kl_stream_write_free(&s), 0);
    kl_event_ctx_free(&g_ev);
}

/* Freed with the check pending: it is cancelled, so the loop never touches the freed stream. */
UTEST(stream_writable, late_install_freed_before_the_check_never_fires) {
    ASSERT_EQ(ev_init(), 0);
    KlStream *s = malloc(sizeof *s); ASSERT_TRUE(s != NULL);
    RW w; Prod p; late_readiness(s, &w, &p, 64);
    refuse_then_drain(s, &w, &p);
    ASSERT_EQ(kl_stream_on_writable(s, prod_writable, &p), 0);
    ASSERT_EQ(kl_stream_write_free(s), 0);
    free(s);
    ASSERT_EQ(kl_timer_next_timeout(&g_ev, 1000), 1000);   /* the check was cancelled, not left due */
    for (int i = 0; i < 4; i++) ASSERT_GE(kl_event_ctx_run(&g_ev, 8, 0), 0);
    ASSERT_EQ(p.fired, 0);
    kl_event_ctx_free(&g_ev);
}

/* The loop torn down with the check pending (a contract breach: free the stream first): the check
 * never fires, and the stream's later free finds nothing to cancel and leaks nothing. */
UTEST(stream_writable, late_install_ctx_teardown_with_the_check_pending) {
    ASSERT_EQ(ev_init(), 0);
    KlStream s; RW w; Prod p; late_readiness(&s, &w, &p, 64);
    refuse_then_drain(&s, &w, &p);
    ASSERT_EQ(kl_stream_on_writable(&s, prod_writable, &p), 0);
    kl_event_ctx_free(&g_ev);
    ASSERT_EQ(p.fired, 0);
    ASSERT_EQ(kl_stream_write_free(&s), 0);
    ASSERT_EQ(p.fired, 0);
}

UTEST(stream_writable, late_install_repeated_keeps_one_check_pending) {
    ASSERT_EQ(ev_init(), 0);
    KlStream s; RW w; Prod p; late_readiness(&s, &w, &p, 64);
    refuse_then_drain(&s, &w, &p);
    for (int i = 0; i < 5; i++) ASSERT_EQ(kl_stream_on_writable(&s, prod_writable, &p), 0);
    ASSERT_EQ(kl_timer_fire(&g_ev), 1);                     /* each install superseded the last */
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ(kl_timer_fire(&g_ev), 0);
    ASSERT_EQ(kl_stream_write_free(&s), 0);
    kl_event_ctx_free(&g_ev);
}

/* A progress point that comes first fires the edge itself; the pending check is then void. */
UTEST(stream_writable, late_install_progress_before_the_check_fires_once) {
    ASSERT_EQ(ev_init(), 0);
    KlStream s; RW w; Prod p; late_readiness(&s, &w, &p, 64);
    refuse_then_drain(&s, &w, &p);
    ASSERT_EQ(kl_stream_on_writable(&s, prod_writable, &p), 0);
    ASSERT_EQ(kl_stream_flush(&s), 0);                      /* an adapter's (empty) flush */
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ(kl_timer_fire(&g_ev), 0);                     /* the check was cancelled, not re-run */
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ(kl_stream_write_free(&s), 0);
    kl_event_ctx_free(&g_ev);
}

/* The deferred callback is refused again: it re-arms for the next progress point, and the check does
 * not reschedule itself (no zero-delay loop without progress). */
UTEST(stream_writable, late_install_refused_again_rearms_without_rescheduling) {
    ASSERT_EQ(ev_init(), 0);
    KlStream s; RW w; Prod p; late_readiness(&s, &w, &p, 64);
    refuse_then_drain(&s, &w, &p);
    p.action = 5; p.wlen = 40; p.rw = &w;
    ASSERT_EQ(kl_stream_on_writable(&s, prod_writable, &p), 0);
    ASSERT_EQ(kl_timer_fire(&g_ev), 1);
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ((int)p.wst, (int)KL_STREAM_WOULD_BLOCK);      /* 40 queued, 40 more do not fit */
    p.action = 0;
    for (int i = 0; i < 3; i++) ASSERT_EQ(kl_timer_fire(&g_ev), 0);   /* nothing rescheduled */
    ASSERT_EQ(p.fired, 1);
    w.mode = 0;
    ASSERT_EQ(kl_stream_flush(&s), 0);                      /* progress: the re-armed edge */
    ASSERT_EQ(p.fired, 2);
    ASSERT_EQ(kl_stream_write_free(&s), 0);
    kl_event_ctx_free(&g_ev);
}

/* The deferred callback begins a close, and its owner frees the stream from on_close: detachment waits
 * for the callback to return, and nothing touches the stream after on_close (ASan). */
static int g_owner_freed;
static void owner_free_on_close(void *ctx) {
    KlStream *s = ctx;
    g_owner_freed = (s->in_writable == 0) ? 1 : -1;        /* never mid-callback */
    (void)kl_stream_write_free(s);
    free(s);
}
UTEST(stream_writable, late_install_callback_closes_and_owner_frees_on_close) {
    ASSERT_EQ(ev_init(), 0);
    KlStream *s = malloc(sizeof *s); ASSERT_TRUE(s != NULL);
    RW w; Prod p;
    g_alloc = kl_allocator_default();
    memset(&w, 0, sizeof w); memset(&p, 0, sizeof p); p.s = s;
    g_closes = 0; g_owner_freed = 0;
    kl_stream_init(s, g_rbuf, sizeof g_rbuf);
    s->ctx = &g_ev;
    kl_stream_write_init(s, &g_alloc, 64);
    kl_stream_set_writer(s, rw_write, &w);
    kl_stream_close_init(s, owner_free_on_close, s);
    refuse_then_drain(s, &w, &p);
    p.action = 2;                                           /* close_begin from the callback */
    ASSERT_EQ(kl_stream_on_writable(s, prod_writable, &p), 0);
    ASSERT_EQ(kl_timer_fire(&g_ev), 1);
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ(g_owner_freed, 1);                            /* detached after the callback returned */
    ASSERT_EQ(kl_timer_fire(&g_ev), 0);
    kl_event_ctx_free(&g_ev);
}

/* A producer that reinstalls its callback on every refusal, with bytes still pending below capacity
 * and the transport stalled: a progress point will come, so nothing is deferred and nothing fires per
 * loop tick (no zero-delay loop without progress). The next flush fires it once. */
typedef struct { KlStream *s; int fired; } Spin;
static const char spin_fill[64];
static void spin_cb(void *ud) {
    Spin *sp = ud; sp->fired++;
    if (kl_stream_write(sp->s, spin_fill, 40) == KL_STREAM_WOULD_BLOCK)
        (void)kl_stream_on_writable(sp->s, spin_cb, sp);   /* install-on-refusal pattern */
}
UTEST(stream_writable, late_install_reinstall_on_refusal_does_not_spin) {
    ASSERT_EQ(ev_init(), 0);
    KlStream s; RW w; Prod p; late_readiness(&s, &w, &p, 64);
    w.mode = 2;                                             /* transport stalled */
    ASSERT_EQ((int)pw(&p, 40), (int)KL_STREAM_ACCEPTED);    /* 40 pending < 64 */
    Spin sp = { &s, 0 };
    ASSERT_EQ((int)kl_stream_write(&s, spin_fill, 40), (int)KL_STREAM_WOULD_BLOCK);
    ASSERT_EQ(kl_stream_on_writable(&s, spin_cb, &sp), 0);
    int total = 0;
    for (int i = 0; i < 100; i++) total += kl_timer_fire(&g_ev);
    ASSERT_EQ(total, 0);                                    /* nothing deferred: progress will come */
    ASSERT_EQ(sp.fired, 0);
    ASSERT_EQ(w.len, (size_t)0);
    w.mode = 1; w.budget = 30;                              /* progress: 10 pending */
    ASSERT_EQ(kl_stream_flush(&s), 1);
    ASSERT_EQ(sp.fired, 1);                                 /* the edge; its retry of 40 fits */
    for (int i = 0; i < 10; i++) total += kl_timer_fire(&g_ev);
    ASSERT_EQ(total, 0);
    ASSERT_EQ(sp.fired, 1);
    (void)kl_stream_on_writable(&s, NULL, NULL);
    ASSERT_EQ(kl_stream_write_free(&s), 0);
    kl_event_ctx_free(&g_ev);
}

/* An adapter's owner hold brackets the deferred fire: a callback that drops the adapter's last
 * reference (as kl_pipe_free does) frees the stream only after the stream's last access. */
typedef struct { KlStream *s; int refs; int freed_in_cb; int in_cb; int released; } Owner;
static void owner_unref(Owner *o) {
    if (--o->refs > 0) return;
    if (o->in_cb) o->freed_in_cb = 1;
    (void)kl_stream_write_free(o->s);
    free(o->s);
    o->s = NULL;
}
static void owner_retain(void *ud)  { ((Owner *)ud)->refs++; }
static void owner_release(void *ud) { Owner *o = ud; o->released++; owner_unref(o); }
static void owner_drop_cb(void *ud) {
    Owner *o = ud;
    o->in_cb = 1;
    (void)kl_stream_cancel(o->s);                           /* what kl_pipe_free does first */
    owner_unref(o);                                         /* ... then it drops the owner ref */
    o->in_cb = 0;
}
static void owner_on_close(void *ud) { (void)ud; g_closes++; }
UTEST(stream_writable, late_install_owner_hold_keeps_the_stream_alive_across_the_callback) {
    ASSERT_EQ(ev_init(), 0);
    KlStream *s = malloc(sizeof *s); ASSERT_TRUE(s != NULL);
    RW w; Prod p;
    g_alloc = kl_allocator_default();
    memset(&w, 0, sizeof w); memset(&p, 0, sizeof p); p.s = s;
    g_closes = 0;
    Owner o = { s, 1, 0, 0, 0 };
    kl_stream_init(s, g_rbuf, sizeof g_rbuf);
    s->ctx = &g_ev;
    kl_stream_write_init(s, &g_alloc, 64);
    kl_stream_set_writer(s, rw_write, &w);
    kl_stream_close_init(s, owner_on_close, &o);
    ASSERT_EQ(kl_stream_set_owner_hold(s, owner_retain, owner_release, &o), 0);
    refuse_then_drain(s, &w, &p);
    ASSERT_EQ(kl_stream_on_writable(s, owner_drop_cb, &o), 0);
    ASSERT_EQ(kl_timer_fire(&g_ev), 1);
    ASSERT_EQ(o.freed_in_cb, 0);                            /* the hold kept it alive */
    ASSERT_EQ(o.released, 1);                               /* ... and its release freed it */
    ASSERT_TRUE(o.s == NULL);
    ASSERT_EQ(g_closes, 1);                                 /* detached before the release */
    kl_event_ctx_free(&g_ev);
}

/* Contract-breach guard: a stream that outlives its loop (it must be freed first) holds a stale
 * timer id. If the ctx storage is re-initialised, ids restart, and the stale id may name another
 * owner's timer; the stream's later free must not cancel it. */
static int g_other_fired;
static void other_timer(void *ud) { (void)ud; g_other_fired++; }
UTEST(stream_writable, late_install_stale_id_cannot_cancel_a_reused_timer) {
    ASSERT_EQ(ev_init(), 0);
    KlStream s; RW w; Prod p; late_readiness(&s, &w, &p, 64);
    refuse_then_drain(&s, &w, &p);
    ASSERT_EQ(kl_stream_on_writable(&s, prod_writable, &p), 0);
    int64_t stale = s.wr_defer_id;
    kl_event_ctx_free(&g_ev);
    ASSERT_EQ(ev_init(), 0);
    g_other_fired = 0;
    int64_t other = -1;
    for (int i = 0; i <= stale && i < 64; i++) other = kl_timer_add(&g_ev, 0, other_timer, NULL);
    ASSERT_EQ(other, stale);                                /* the id was reused */
    ASSERT_EQ(kl_stream_write_free(&s), 0);                 /* must not cancel it */
    ASSERT_EQ(kl_timer_fire(&g_ev), (int)stale + 1);
    ASSERT_EQ(g_other_fired, (int)stale + 1);
    ASSERT_EQ(p.fired, 0);
    kl_event_ctx_free(&g_ev);
}

/* A stream with no loop (ctx NULL, as the mock streams above) cannot defer: the late install is
 * accepted and the refusal stays armed for the next progress point, if any. */
UTEST(stream_writable, late_install_without_a_loop_defers_nothing) {
    KlStream s; RW w; Prod p; late_readiness(&s, &w, &p, 64);
    s.ctx = NULL;
    refuse_then_drain(&s, &w, &p);
    ASSERT_EQ(kl_stream_on_writable(&s, prod_writable, &p), 0);
    ASSERT_EQ(p.fired, 0);
    ASSERT_EQ(kl_stream_write_free(&s), 0);
}

/* ── Live: a producer driven only by the edge, over a real socket ───────────────────────────── */

static int make_pair(KlSocketHandle *end, KlSocketHandle *peer) {
    KlSocketHandle lis = kl_sockdef_socket(AF_INET, SOCK_STREAM, 0);
    if (!kl_handle_valid(lis)) return -1;
    uint8_t lo[4] = { 127, 0, 0, 1 };
    KlSockAddr addr; kl_sockaddr_from_ipv4(&addr, lo, 0);
    KlSockAddr bound;
    KlSocketHandle cli = KL_INVALID_SOCKET, srv = KL_INVALID_SOCKET;
    if (kl_sockdef_bind(lis, &addr) < 0 || kl_sockdef_listen(lis, 1) < 0) goto fail;
    if (kl_sockdef_get_local_addr(lis, &bound) < 0) goto fail;
    cli = kl_sockdef_socket(AF_INET, SOCK_STREAM, 0);
    if (!kl_handle_valid(cli)) goto fail;
    if (kl_sockdef_connect(cli, &bound) < 0) goto fail;
    srv = kl_sockdef_accept(lis, NULL);
    if (!kl_handle_valid(srv)) goto fail;
    kl_sockdef_close(lis);
    *end = cli; *peer = srv;
    return 0;
fail:
    if (kl_handle_valid(lis)) kl_sockdef_close(lis);
    if (kl_handle_valid(cli)) kl_sockdef_close(cli);
    return -1;
}
static kl_ssize_t sock_writer(const char *data, size_t len, void *ctx) {
    KlSocketHandle fd = *(KlSocketHandle *)ctx;
    kl_ssize_t n = kl_sockdef_send(fd, data, len);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return n;
}
static unsigned char pat(size_t i) { return (unsigned char)((i * 131u + (i >> 11)) & 0xFF); }

typedef struct { int ready; int edges; } Live;
static void live_writable(void *ctx) { Live *l = ctx; l->ready = 1; l->edges++; }

UTEST(stream_writable, live_socket_producer_resumes_only_on_the_edge) {
    KlAllocator a = kl_allocator_default();
    KlSocketHandle end, peer;
    ASSERT_EQ(make_pair(&end, &peer), 0);
    ASSERT_EQ(kl_sockdef_set_nonblocking(end), 0);
    ASSERT_EQ(kl_sockdef_set_nonblocking(peer), 0);

    KlStream s;
    ASSERT_EQ(kl_stream_init(&s, g_rbuf, sizeof g_rbuf), 0);
    ASSERT_EQ(kl_stream_write_init(&s, &a, 64 * 1024), 0);
    ASSERT_EQ(kl_stream_set_writer(&s, sock_writer, &end), 0);
    Live l = { 1, 0 };
    ASSERT_EQ(kl_stream_on_writable(&s, live_writable, &l), 0);

    const size_t total = 16u << 20;          /* far beyond the kernel buffers + the 64 KiB queue */
    static char chunk[16 * 1024];
    static char in[64 * 1024];
    size_t sent = 0, got = 0; int would_block = 0, bad = 0;
    for (long iter = 0; iter < 20000000L && got < total; iter++) {
        /* The producer writes only while it holds a go-ahead: initially, then after each edge. */
        while (l.ready && sent < total) {
            size_t n = total - sent < sizeof chunk ? total - sent : sizeof chunk;
            for (size_t i = 0; i < n; i++) chunk[i] = (char)pat(sent + i);
            KlStreamWriteStatus ws = kl_stream_write(&s, chunk, n);
            if (ws == KL_STREAM_WOULD_BLOCK) { would_block++; l.ready = 0; break; }
            ASSERT_EQ((int)ws, (int)KL_STREAM_ACCEPTED);
            sent += n;
        }
        /* The peer reads a little at a time, so the edge has to fire many times. */
        kl_ssize_t k = kl_sockdef_recv(peer, in, (iter & 1) ? sizeof in : 1500);
        for (kl_ssize_t i = 0; i < k; i++) if ((unsigned char)in[i] != pat(got + (size_t)i)) bad++;
        if (k > 0) got += (size_t)k;
        /* The adapter's writable signal. */
        ASSERT_GE(kl_stream_flush(&s), 0);
    }
    ASSERT_EQ(sent, total);
    ASSERT_EQ(got, total);
    ASSERT_EQ(bad, 0);                          /* every byte, in order */
    ASSERT_GT(would_block, 10);                 /* the queue really pushed back, repeatedly */
    ASSERT_EQ(l.edges, would_block);            /* each block resumed by exactly one edge */

    ASSERT_EQ(kl_stream_write_free(&s), 0);
    kl_sockdef_close(peer);
    kl_sockdef_close(end);
}

UTEST_MAIN();
