/*
 * test_stream_writable.c: the KlStream writable-again edge (kl_stream_on_writable), with mock
 * readiness-writer and completion-submit hooks (no live sockets).
 *
 * The contract under test, one rule per case:
 *   - only a WOULD_BLOCK arms it; bytes draining alone never fire it;
 *   - an ACCEPTED write disarms it;
 *   - it fires once per arming, from kl_stream_flush / kl_stream_on_write_complete only, never from
 *     inside kl_stream_write (even when the submit completes inline);
 *   - "full" is the stream's TOTAL pending count: in-flight bytes a copying backend has already
 *     consumed from the queue still hold it back;
 *   - a terminal write failure wakes an armed producer once, and its retry reports ERROR;
 *   - it never fires once a close has begun;
 *   - the callback may write, and may begin a close whose detachment waits for it to return.
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
#include "../src/socket.h"        /* kl_sockdef_*: the default socket seam (live section) */
#include "net_compat.h"           /* AF_INET / SOCK_STREAM */
#include <errno.h>
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

/* Completion submitter. `inline_ok` completes the send synchronously, inside the submit. */
typedef struct { KlStream *s; int submits; size_t last_len; int inline_ok; } CS;
static int cs_submit(void *ctx, const char *data, size_t len) {
    CS *c = ctx; (void)data;
    c->submits++; c->last_len = len;
    if (c->inline_ok) (void)kl_stream_on_write_complete(c->s, 1);
    return 0;
}

/* The producer: counts edges, records whether one arrived inside kl_stream_write, and can act. */
typedef struct {
    KlStream *s;
    int fired;
    int in_write;          /* set by the test around its own kl_stream_write calls */
    int fired_in_write;
    int action;            /* 0 none, 1 write `wlen` bytes, 2 close_begin, 3 cancel,
                            * 4 stall the readiness writer `rw` then write `wlen` bytes */
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
    kl_stream_write_free(&s);
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
    kl_stream_write_free(&s);
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
    kl_stream_write_free(&s);
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
    kl_stream_write_free(&s);
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
    kl_stream_write_free(&s);
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
    kl_stream_write_free(&s);
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
    kl_stream_write_free(&s);
}

/* ── Never from inside kl_stream_write ──────────────────────────────────────────────────────── */

UTEST(stream_writable, inline_completion_inside_write_does_not_fire) {
    KlStream s; CS c; Prod p; completion(&s, &c, &p, 64, /*copying=*/1);
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 8), (int)KL_STREAM_WOULD_BLOCK);  /* armed */
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);      /* 64 still in flight: no edge yet */
    ASSERT_EQ(p.fired, 0);
    /* From here every submit completes synchronously, inside the submit. A write the producer makes
     * while blocked-but-not-yet-woken is accepted into the free queue space; its pump runs the
     * outstanding completions inline. None of that may call back into the producer. */
    c.inline_ok = 1;
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);      /* drains everything, inline chain */
    ASSERT_EQ(p.fired, 1);                                  /* from the completion, not a write */
    ASSERT_EQ((int)pw(&p, 8), (int)KL_STREAM_ACCEPTED);    /* inline completion inside the write */
    ASSERT_EQ(p.fired, 1);
    ASSERT_EQ(p.fired_in_write, 0);
    kl_stream_write_free(&s);
}

UTEST(stream_writable, accepted_write_with_inline_completion_while_armed) {
    KlStream s; CS c; Prod p; completion(&s, &c, &p, 64, /*copying=*/1);
    ASSERT_EQ((int)pw(&p, 64), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 60), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ((int)pw(&p, 8), (int)KL_STREAM_WOULD_BLOCK);  /* armed */
    ASSERT_EQ((int)pw(&p, 4), (int)KL_STREAM_ACCEPTED);     /* fits: disarms before any pump */
    c.inline_ok = 1;
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);
    ASSERT_EQ(p.fired, 0);
    kl_stream_write_free(&s);
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
    kl_stream_write_free(&s);
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
    kl_stream_write_free(&s);
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
    kl_stream_write_free(&s);
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
    kl_stream_write_free(&s);
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
    kl_stream_write_free(&s);
}

UTEST(stream_writable, failure_without_a_blocked_producer_is_silent) {
    KlStream s; CS c; Prod p; completion(&s, &c, &p, 64, /*copying=*/1);
    ASSERT_EQ((int)pw(&p, 10), (int)KL_STREAM_ACCEPTED);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 0), -1);
    ASSERT_EQ(p.fired, 0);
    kl_stream_write_free(&s);
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
    kl_stream_write_free(&s);
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
    kl_stream_write_free(&s);
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
    kl_stream_write_free(&s);
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
