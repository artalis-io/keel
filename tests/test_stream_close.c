/*
 * test_stream_close.c — Phase-B step 3: graceful-close / confirmed-detachment lifecycle
 * (src/stream_close.h), exercised in isolation over the step-2A write machinery and the step-2B
 * read machinery with mock hooks (no live sockets).
 *
 * The single invariant under test: on_close (detachment) fires EXACTLY ONCE, and ONLY after BOTH
 * the receive (recv_inflight == 0) and the send (send_inflight == 0) operations are physically
 * retired — a merely logical close is never enough. Covers graceful drain, abortive cancel with
 * synchronous AND asynchronous cancel completion (obeying the arm-trampoline discipline), order
 * independence of the two retirements, queue draining, readiness flush retirement, write refusal
 * while closing, idempotence, and graceful→abortive escalation.
 */
#include "utest.h"
#include <keel/http_connection.h>
#include <keel/allocator.h>
#include "../src/stream_write.h"
#include "../src/stream_read.h"
#include "../src/stream_close.h"
#include <string.h>
#include <stdlib.h>

static KlAllocator g_alloc;   /* stable for the lifetime of the write queue (kl_drain stores it) */

/* ── Combined mock: read hooks + write submit + cancel hooks + detachment ──────────────────── */
typedef struct {
    KlStream *s;
    int  closed;                 /* on_close count (must end at exactly 1) */
    int  deliver_calls;
    int  arm_calls, disarm_calls;
    int  submit_calls;
    int  cancel_recv_calls, cancel_send_calls;
    int  cancel_recv_sync;       /* cancel_recv completes the recv inline (terminal) */
    int  cancel_send_sync;       /* cancel_send completes the send inline (failure/cancelled) */
    int  cancel_send_reentrant;  /* cancel_send reentrantly calls kl_stream_cancel(s) */
    int  closed_at_cancel;       /* snapshot of `closed` observed from inside a cancel hook */
    int  wblock;                 /* readiness writer: 1 = would-block (buffer), 0 = consume */
    size_t wsent;
} LC;

static void lc_on_close(void *ctx) { LC *l = ctx; l->closed++; }
static void lc_deliver(void *ctx, const char *b, size_t n, int ok) {
    (void)b; (void)n; (void)ok; LC *l = ctx; l->deliver_calls++;
}
static int  lc_arm(void *ctx)    { LC *l = ctx; l->arm_calls++; return 0; }
static void lc_disarm(void *ctx) { LC *l = ctx; l->disarm_calls++; }
static int  lc_submit(void *ctx, const char *d, size_t n) {
    (void)d; (void)n; LC *l = ctx; l->submit_calls++; return 0;   /* submitted; test drives completion */
}
static kl_ssize_t lc_write(const char *d, size_t n, void *ctx) {
    (void)d; LC *l = ctx; if (l->wblock) return 0; l->wsent += n; return (kl_ssize_t)n;
}
static int lc_cancel_recv(void *ctx) {
    LC *l = ctx; l->cancel_recv_calls++;
    l->closed_at_cancel = l->closed;                  /* must still be 0 mid-cancel (deferred) */
    if (l->cancel_recv_sync) kl_stream_on_recv(l->s, 0, 0);       /* synchronous cancelled recv */
    return 0;
}
static int lc_cancel_send(void *ctx) {
    LC *l = ctx; l->cancel_send_calls++;
    l->closed_at_cancel = l->closed;
    if (l->cancel_send_sync) kl_stream_on_write_complete(l->s, 0); /* synchronous cancelled send */
    if (l->cancel_send_reentrant) kl_stream_cancel(l->s);         /* reentrant close from the hook */
    return 0;
}

/* Build a stream with a heap read_buf and a preallocated write queue. */
static void mk(KlStream *s, char **buf, LC *l) {
    g_alloc = kl_allocator_default();
    memset(s, 0, sizeof(*s));
    memset(l, 0, sizeof(*l));
    l->s = s;
    *buf = malloc(256);
    s->read_buf = *buf;
    s->read_cap = 256;
    kl_stream_write_init(s, &g_alloc, 4096);
}
static void mk_free(KlStream *s, char *buf) { kl_stream_write_free(s); free(buf); }

/* ── Tests ─────────────────────────────────────────────────────────────────────────────────── */

UTEST(stream_close, graceful_no_ops_detaches_immediately) {
    KlStream s; char *buf; LC l; mk(&s, &buf, &l);
    ASSERT_EQ(kl_stream_close_init(&s, lc_on_close, &l), 0);
    ASSERT_EQ(kl_stream_close_state(&s), KL_STREAM_STATE_OPEN);
    ASSERT_EQ(kl_stream_close_begin(&s), 0);           /* nothing outstanding, nothing queued */
    ASSERT_EQ(l.closed, 1);                            /* detaches synchronously */
    ASSERT_EQ(kl_stream_close_state(&s), KL_STREAM_STATE_CLOSED);
    ASSERT_EQ(kl_stream_is_detached(&s), 1);
    mk_free(&s, buf);
}

UTEST(stream_close, graceful_waits_for_recv_completion) {
    KlStream s; char *buf; LC l; mk(&s, &buf, &l);
    ASSERT_EQ(kl_stream_read_init(&s, /*completion=*/1, lc_deliver, lc_arm, lc_disarm, &l), 0);
    ASSERT_EQ(kl_stream_close_init(&s, lc_on_close, &l), 0);
    ASSERT_EQ(kl_stream_read_start(&s), 0);            /* recv posted (in flight) */

    ASSERT_EQ(kl_stream_close_begin(&s), 0);           /* logical read close; recv still physical */
    ASSERT_EQ(l.closed, 0);                            /* NOT detached — recv op outstanding */
    ASSERT_EQ(kl_stream_close_state(&s), KL_STREAM_STATE_CLOSING);

    kl_stream_on_recv(&s, 5, 1);                       /* completion arrives after close → dropped */
    ASSERT_EQ(l.deliver_calls, 0);                     /* never delivered after close */
    ASSERT_EQ(l.closed, 1);                            /* recv retired → detached */
    ASSERT_EQ(kl_stream_is_detached(&s), 1);
    mk_free(&s, buf);
}

UTEST(stream_close, graceful_waits_for_send_completion) {
    KlStream s; char *buf; LC l; mk(&s, &buf, &l);
    ASSERT_EQ(kl_stream_set_submit(&s, lc_submit, &l, /*copying=*/0), 0);
    ASSERT_EQ(kl_stream_close_init(&s, lc_on_close, &l), 0);
    ASSERT_EQ((int)kl_stream_write(&s, "hello", 5), KL_STREAM_ACCEPTED);
    ASSERT_EQ(l.submit_calls, 1);                      /* one send in flight */

    ASSERT_EQ(kl_stream_close_begin(&s), 0);
    ASSERT_EQ(l.closed, 0);                            /* send op still outstanding */

    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);  /* send retires, queue empties */
    ASSERT_EQ(l.closed, 1);                            /* detached */
    mk_free(&s, buf);
}

UTEST(stream_close, graceful_waits_for_both_recv_then_send) {
    KlStream s; char *buf; LC l; mk(&s, &buf, &l);
    ASSERT_EQ(kl_stream_read_init(&s, 1, lc_deliver, lc_arm, lc_disarm, &l), 0);
    ASSERT_EQ(kl_stream_set_submit(&s, lc_submit, &l, 0), 0);
    ASSERT_EQ(kl_stream_close_init(&s, lc_on_close, &l), 0);
    ASSERT_EQ(kl_stream_read_start(&s), 0);
    ASSERT_EQ((int)kl_stream_write(&s, "hi", 2), KL_STREAM_ACCEPTED);

    ASSERT_EQ(kl_stream_close_begin(&s), 0);
    ASSERT_EQ(l.closed, 0);
    kl_stream_on_recv(&s, 3, 1);                       /* recv retires first */
    ASSERT_EQ(l.closed, 0);                            /* send still outstanding */
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);  /* send retires second */
    ASSERT_EQ(l.closed, 1);                            /* now detached */
    mk_free(&s, buf);
}

UTEST(stream_close, graceful_waits_for_both_send_then_recv) {
    KlStream s; char *buf; LC l; mk(&s, &buf, &l);
    ASSERT_EQ(kl_stream_read_init(&s, 1, lc_deliver, lc_arm, lc_disarm, &l), 0);
    ASSERT_EQ(kl_stream_set_submit(&s, lc_submit, &l, 0), 0);
    ASSERT_EQ(kl_stream_close_init(&s, lc_on_close, &l), 0);
    ASSERT_EQ(kl_stream_read_start(&s), 0);
    ASSERT_EQ((int)kl_stream_write(&s, "hi", 2), KL_STREAM_ACCEPTED);

    ASSERT_EQ(kl_stream_close_begin(&s), 0);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);  /* send retires first */
    ASSERT_EQ(l.closed, 0);                            /* recv still outstanding */
    kl_stream_on_recv(&s, 3, 1);                       /* recv retires second */
    ASSERT_EQ(l.closed, 1);
    mk_free(&s, buf);
}

UTEST(stream_close, graceful_drains_whole_queue_before_detach) {
    KlStream s; char *buf; LC l; mk(&s, &buf, &l);
    ASSERT_EQ(kl_stream_set_submit(&s, lc_submit, &l, /*copying=*/0), 0);  /* referencing */
    ASSERT_EQ(kl_stream_close_init(&s, lc_on_close, &l), 0);
    ASSERT_EQ((int)kl_stream_write(&s, "AAAA", 4), KL_STREAM_ACCEPTED);   /* batch1 submitted */
    ASSERT_EQ((int)kl_stream_write(&s, "BBBB", 4), KL_STREAM_ACCEPTED);   /* batch2 queued behind */
    ASSERT_EQ(l.submit_calls, 1);

    ASSERT_EQ(kl_stream_close_begin(&s), 0);           /* graceful: must drain batch2 too */
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);  /* batch1 done → pump batch2 */
    ASSERT_EQ(l.submit_calls, 2);
    ASSERT_EQ(l.closed, 0);                            /* queue not yet empty */
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);  /* batch2 done → queue empty */
    ASSERT_EQ(l.closed, 1);                            /* fully drained → detached */
    mk_free(&s, buf);
}

UTEST(stream_close, readiness_recv_disarmed_then_flush_retires_send) {
    KlStream s; char *buf; LC l; mk(&s, &buf, &l);
    ASSERT_EQ(kl_stream_read_init(&s, /*completion=*/0, lc_deliver, lc_arm, lc_disarm, &l), 0);
    ASSERT_EQ(kl_stream_set_writer(&s, lc_write, &l), 0);
    ASSERT_EQ(kl_stream_close_init(&s, lc_on_close, &l), 0);
    ASSERT_EQ(kl_stream_read_start(&s), 0);            /* READ interest armed */
    l.wblock = 1;                                      /* writer would-block → bytes buffered */
    ASSERT_EQ((int)kl_stream_write(&s, "queued", 6), KL_STREAM_ACCEPTED);
    ASSERT_GT((int)kl_stream_write_pending(&s), 0);

    ASSERT_EQ(kl_stream_close_begin(&s), 0);
    ASSERT_EQ(l.disarm_calls, 1);                      /* readiness recv retired via disarm */
    ASSERT_EQ(l.closed, 0);                            /* graceful: queue still pending */

    l.wblock = 0;                                      /* socket writable now */
    ASSERT_EQ(kl_stream_flush(&s), 0);                 /* drains fully */
    ASSERT_EQ(l.closed, 1);                            /* write side retired → detached */
    mk_free(&s, buf);
}

UTEST(stream_close, closing_refuses_new_writes) {
    KlStream s; char *buf; LC l; mk(&s, &buf, &l);
    ASSERT_EQ(kl_stream_set_submit(&s, lc_submit, &l, 0), 0);
    ASSERT_EQ(kl_stream_close_init(&s, lc_on_close, &l), 0);
    ASSERT_EQ((int)kl_stream_write(&s, "x", 1), KL_STREAM_ACCEPTED);
    ASSERT_EQ(kl_stream_close_begin(&s), 0);
    ASSERT_EQ((int)kl_stream_write(&s, "y", 1), KL_STREAM_CLOSED);   /* new write refused while closing */
    ASSERT_EQ(l.submit_calls, 1);                              /* the refused write never submitted */
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);          /* retire in-flight "x" so the write
                                                                * queue frees (write_free refuses
                                                                * while a send is in flight) */
    mk_free(&s, buf);
}

UTEST(stream_close, abortive_sync_cancel_detaches_exactly_once) {
    KlStream s; char *buf; LC l; mk(&s, &buf, &l);
    ASSERT_EQ(kl_stream_read_init(&s, 1, lc_deliver, lc_arm, lc_disarm, &l), 0);
    ASSERT_EQ(kl_stream_set_submit(&s, lc_submit, &l, 0), 0);
    ASSERT_EQ(kl_stream_close_init(&s, lc_on_close, &l), 0);
    ASSERT_EQ(kl_stream_set_cancel(&s, lc_cancel_recv, lc_cancel_send), 0);
    ASSERT_EQ(kl_stream_read_start(&s), 0);
    ASSERT_EQ((int)kl_stream_write(&s, "z", 1), KL_STREAM_ACCEPTED);
    l.cancel_recv_sync = 1; l.cancel_send_sync = 1;   /* both cancels retire their op inline */

    ASSERT_EQ(kl_stream_cancel(&s), 0);
    ASSERT_EQ(l.cancel_recv_calls, 1);
    ASSERT_EQ(l.cancel_send_calls, 1);
    ASSERT_EQ(l.closed_at_cancel, 0);                 /* NOT detached mid-cancel (deferred) */
    ASSERT_EQ(l.closed, 1);                           /* detached exactly once after unwind */
    ASSERT_EQ(kl_stream_is_detached(&s), 1);
    mk_free(&s, buf);
}

UTEST(stream_close, abortive_async_cancel_detaches_on_later_completions) {
    KlStream s; char *buf; LC l; mk(&s, &buf, &l);
    ASSERT_EQ(kl_stream_read_init(&s, 1, lc_deliver, lc_arm, lc_disarm, &l), 0);
    ASSERT_EQ(kl_stream_set_submit(&s, lc_submit, &l, 0), 0);
    ASSERT_EQ(kl_stream_close_init(&s, lc_on_close, &l), 0);
    ASSERT_EQ(kl_stream_set_cancel(&s, lc_cancel_recv, lc_cancel_send), 0);
    ASSERT_EQ(kl_stream_read_start(&s), 0);
    ASSERT_EQ((int)kl_stream_write(&s, "z", 1), KL_STREAM_ACCEPTED);
    /* async cancels: hooks just request, ops complete later */

    ASSERT_EQ(kl_stream_cancel(&s), 0);
    ASSERT_EQ(l.cancel_recv_calls, 1);
    ASSERT_EQ(l.cancel_send_calls, 1);
    ASSERT_EQ(l.closed, 0);                           /* ops still physically outstanding */

    kl_stream_on_recv(&s, 0, 0);                      /* cancelled recv completes */
    ASSERT_EQ(l.closed, 0);                           /* send still outstanding */
    ASSERT_EQ(kl_stream_on_write_complete(&s, 0), -1);/* cancelled send completes (failure) */
    ASSERT_EQ(l.closed, 1);                           /* both retired → detached */
    mk_free(&s, buf);
}

UTEST(stream_close, abortive_does_not_wait_for_queued_bytes) {
    KlStream s; char *buf; LC l; mk(&s, &buf, &l);
    ASSERT_EQ(kl_stream_set_submit(&s, lc_submit, &l, 0), 0);
    ASSERT_EQ(kl_stream_close_init(&s, lc_on_close, &l), 0);
    ASSERT_EQ(kl_stream_set_cancel(&s, NULL, lc_cancel_send), 0);
    ASSERT_EQ((int)kl_stream_write(&s, "AAAA", 4), KL_STREAM_ACCEPTED);   /* batch1 in flight */
    ASSERT_EQ((int)kl_stream_write(&s, "BBBB", 4), KL_STREAM_ACCEPTED);   /* batch2 queued, unsubmitted */
    l.cancel_send_sync = 1;

    ASSERT_EQ(kl_stream_cancel(&s), 0);              /* abortive: cancels in-flight send inline */
    ASSERT_EQ(l.cancel_send_calls, 1);
    ASSERT_EQ(l.closed, 1);                          /* detaches without draining batch2 */
    ASSERT_GT((int)kl_stream_write_pending(&s), 0);  /* queued bytes remain (owner frees them) */
    mk_free(&s, buf);
}

UTEST(stream_close, abortive_no_inflight_skips_cancel_hooks) {
    KlStream s; char *buf; LC l; mk(&s, &buf, &l);
    ASSERT_EQ(kl_stream_close_init(&s, lc_on_close, &l), 0);
    ASSERT_EQ(kl_stream_set_cancel(&s, lc_cancel_recv, lc_cancel_send), 0);
    ASSERT_EQ(kl_stream_cancel(&s), 0);              /* nothing outstanding */
    ASSERT_EQ(l.cancel_recv_calls, 0);              /* hooks only called when an op is in flight */
    ASSERT_EQ(l.cancel_send_calls, 0);
    ASSERT_EQ(l.closed, 1);
    mk_free(&s, buf);
}

UTEST(stream_close, graceful_then_cancel_escalation) {
    KlStream s; char *buf; LC l; mk(&s, &buf, &l);
    ASSERT_EQ(kl_stream_set_submit(&s, lc_submit, &l, 0), 0);
    ASSERT_EQ(kl_stream_close_init(&s, lc_on_close, &l), 0);
    ASSERT_EQ(kl_stream_set_cancel(&s, NULL, lc_cancel_send), 0);
    ASSERT_EQ((int)kl_stream_write(&s, "z", 1), KL_STREAM_ACCEPTED);

    ASSERT_EQ(kl_stream_close_begin(&s), 0);         /* graceful: waits on the send */
    ASSERT_EQ(l.closed, 0);
    ASSERT_EQ(l.cancel_send_calls, 0);               /* graceful does not cancel */

    l.cancel_send_sync = 1;
    ASSERT_EQ(kl_stream_cancel(&s), 0);              /* escalate to abortive */
    ASSERT_EQ(l.cancel_send_calls, 1);               /* now the send is cancelled */
    ASSERT_EQ(l.closed, 1);                          /* detached once */
    mk_free(&s, buf);
}

UTEST(stream_close, idempotent_double_begin_and_after_closed) {
    KlStream s; char *buf; LC l; mk(&s, &buf, &l);
    ASSERT_EQ(kl_stream_close_init(&s, lc_on_close, &l), 0);
    ASSERT_EQ(kl_stream_close_begin(&s), 0);
    ASSERT_EQ(l.closed, 1);
    ASSERT_EQ(kl_stream_close_begin(&s), 0);         /* already CLOSED — no-op */
    ASSERT_EQ(kl_stream_cancel(&s), 0);              /* also no-op after CLOSED */
    ASSERT_EQ(l.closed, 1);                          /* on_close fired exactly once */
    mk_free(&s, buf);
}

UTEST(stream_close, double_begin_while_waiting_fires_once) {
    KlStream s; char *buf; LC l; mk(&s, &buf, &l);
    ASSERT_EQ(kl_stream_set_submit(&s, lc_submit, &l, 0), 0);
    ASSERT_EQ(kl_stream_close_init(&s, lc_on_close, &l), 0);
    ASSERT_EQ((int)kl_stream_write(&s, "z", 1), KL_STREAM_ACCEPTED);
    ASSERT_EQ(kl_stream_close_begin(&s), 0);
    ASSERT_EQ(kl_stream_close_begin(&s), 0);         /* second begin while CLOSING — no-op */
    ASSERT_EQ(l.closed, 0);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);
    ASSERT_EQ(l.closed, 1);                          /* still exactly once */
    mk_free(&s, buf);
}

UTEST(stream_close, abortive_repeat_cancel_requests_once_per_op) {
    /* Two kl_stream_cancel() calls while BOTH ops remain outstanding → one invocation per hook. */
    KlStream s; char *buf; LC l; mk(&s, &buf, &l);
    ASSERT_EQ(kl_stream_read_init(&s, 1, lc_deliver, lc_arm, lc_disarm, &l), 0);
    ASSERT_EQ(kl_stream_set_submit(&s, lc_submit, &l, 0), 0);
    ASSERT_EQ(kl_stream_close_init(&s, lc_on_close, &l), 0);
    ASSERT_EQ(kl_stream_set_cancel(&s, lc_cancel_recv, lc_cancel_send), 0);
    ASSERT_EQ(kl_stream_read_start(&s), 0);
    ASSERT_EQ((int)kl_stream_write(&s, "z", 1), KL_STREAM_ACCEPTED);
    /* async cancels: hooks only request, ops stay outstanding */

    ASSERT_EQ(kl_stream_cancel(&s), 0);
    ASSERT_EQ(l.cancel_recv_calls, 1);
    ASSERT_EQ(l.cancel_send_calls, 1);
    ASSERT_EQ(kl_stream_cancel(&s), 0);              /* both ops still outstanding — no re-request */
    ASSERT_EQ(l.cancel_recv_calls, 1);
    ASSERT_EQ(l.cancel_send_calls, 1);
    ASSERT_EQ(l.closed, 0);

    kl_stream_on_recv(&s, 0, 0);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 0), -1);
    ASSERT_EQ(l.closed, 1);                          /* detached once both retired */
    mk_free(&s, buf);
}

UTEST(stream_close, recv_retires_between_cancels_send_not_repeated) {
    /* Receive retires between the two cancel calls → send cancellation still isn't repeated. */
    KlStream s; char *buf; LC l; mk(&s, &buf, &l);
    ASSERT_EQ(kl_stream_read_init(&s, 1, lc_deliver, lc_arm, lc_disarm, &l), 0);
    ASSERT_EQ(kl_stream_set_submit(&s, lc_submit, &l, 0), 0);
    ASSERT_EQ(kl_stream_close_init(&s, lc_on_close, &l), 0);
    ASSERT_EQ(kl_stream_set_cancel(&s, lc_cancel_recv, lc_cancel_send), 0);
    ASSERT_EQ(kl_stream_read_start(&s), 0);
    ASSERT_EQ((int)kl_stream_write(&s, "z", 1), KL_STREAM_ACCEPTED);

    ASSERT_EQ(kl_stream_cancel(&s), 0);
    ASSERT_EQ(l.cancel_recv_calls, 1);
    ASSERT_EQ(l.cancel_send_calls, 1);

    kl_stream_on_recv(&s, 0, 0);                     /* recv retires between the two cancels */
    ASSERT_EQ(l.closed, 0);                          /* send still outstanding */

    ASSERT_EQ(kl_stream_cancel(&s), 0);             /* recv gone; send already requested */
    ASSERT_EQ(l.cancel_recv_calls, 1);              /* not re-requested (op already retired) */
    ASSERT_EQ(l.cancel_send_calls, 1);              /* not repeated (same op still outstanding) */

    ASSERT_EQ(kl_stream_on_write_complete(&s, 0), -1);
    ASSERT_EQ(l.closed, 1);
    mk_free(&s, buf);
}

UTEST(stream_close, sync_cancel_then_reentrant_cancel_fires_once) {
    /* A cancel hook that synchronously retires its op AND reentrantly calls kl_stream_cancel: the
     * depth counter keeps finalization deferred until the outermost frame unwinds → on_close once,
     * never mid-cancel, and no duplicate cancel request. */
    KlStream s; char *buf; LC l; mk(&s, &buf, &l);
    ASSERT_EQ(kl_stream_set_submit(&s, lc_submit, &l, 0), 0);
    ASSERT_EQ(kl_stream_close_init(&s, lc_on_close, &l), 0);
    ASSERT_EQ(kl_stream_set_cancel(&s, NULL, lc_cancel_send), 0);
    ASSERT_EQ((int)kl_stream_write(&s, "z", 1), KL_STREAM_ACCEPTED);
    l.cancel_send_sync = 1; l.cancel_send_reentrant = 1;

    ASSERT_EQ(kl_stream_cancel(&s), 0);
    ASSERT_EQ(l.cancel_send_calls, 1);              /* reentrant call did NOT re-invoke the hook */
    ASSERT_EQ(l.closed_at_cancel, 0);              /* not detached mid-cancel (deferred by depth) */
    ASSERT_EQ(l.closed, 1);                        /* detached exactly once after unwind */
    mk_free(&s, buf);
}

UTEST(stream_close, escalation_then_repeat_abortive_no_repeat) {
    /* Graceful → abortive escalation, then ANOTHER abortive call → cancel issued exactly once. */
    KlStream s; char *buf; LC l; mk(&s, &buf, &l);
    ASSERT_EQ(kl_stream_set_submit(&s, lc_submit, &l, 0), 0);
    ASSERT_EQ(kl_stream_close_init(&s, lc_on_close, &l), 0);
    ASSERT_EQ(kl_stream_set_cancel(&s, NULL, lc_cancel_send), 0);
    ASSERT_EQ((int)kl_stream_write(&s, "z", 1), KL_STREAM_ACCEPTED);

    ASSERT_EQ(kl_stream_close_begin(&s), 0);        /* graceful: no cancel */
    ASSERT_EQ(l.cancel_send_calls, 0);
    ASSERT_EQ(kl_stream_cancel(&s), 0);             /* escalate → cancel once */
    ASSERT_EQ(l.cancel_send_calls, 1);
    ASSERT_EQ(kl_stream_cancel(&s), 0);             /* repeat abortive → no re-request */
    ASSERT_EQ(l.cancel_send_calls, 1);
    ASSERT_EQ(l.closed, 0);

    ASSERT_EQ(kl_stream_on_write_complete(&s, 0), -1);
    ASSERT_EQ(l.closed, 1);
    mk_free(&s, buf);
}

UTEST(stream_close, set_cancel_frozen_once_closing) {
    KlStream s; char *buf; LC l; mk(&s, &buf, &l);
    ASSERT_EQ(kl_stream_set_submit(&s, lc_submit, &l, 0), 0);
    ASSERT_EQ(kl_stream_close_init(&s, lc_on_close, &l), 0);
    ASSERT_EQ(kl_stream_set_cancel(&s, NULL, lc_cancel_send), 0);   /* OK while OPEN */
    ASSERT_EQ((int)kl_stream_write(&s, "z", 1), KL_STREAM_ACCEPTED);
    ASSERT_EQ(kl_stream_close_begin(&s), 0);                        /* now CLOSING */
    ASSERT_EQ(kl_stream_set_cancel(&s, lc_cancel_recv, lc_cancel_send), -1); /* frozen */
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);          /* retire in-flight "z" so the write
                                                                * queue frees (not in flight) */
    mk_free(&s, buf);
}

UTEST(stream_close, init_rejects_double_and_null) {
    KlStream s; char *buf; LC l; mk(&s, &buf, &l);
    ASSERT_EQ(kl_stream_close_init(&s, NULL, &l), -1);       /* on_close required */
    ASSERT_EQ(kl_stream_close_init(&s, lc_on_close, &l), 0);
    ASSERT_EQ(kl_stream_close_init(&s, lc_on_close, &l), -1);/* double init rejected */
    ASSERT_EQ(kl_stream_set_cancel(&s, lc_cancel_recv, lc_cancel_send), 0);
    mk_free(&s, buf);
}

UTEST(stream_close, uninited_lifecycle_is_safe) {
    KlStream s; char *buf; LC l; mk(&s, &buf, &l);   /* no close_init */
    ASSERT_EQ(kl_stream_close_begin(&s), -1);
    ASSERT_EQ(kl_stream_cancel(&s), -1);
    ASSERT_EQ(kl_stream_set_cancel(&s, NULL, NULL), -1);
    ASSERT_EQ(kl_stream_close_state(&s), KL_STREAM_STATE_OPEN);
    ASSERT_EQ(kl_stream_is_detached(&s), 0);
    mk_free(&s, buf);
}

UTEST_MAIN();
