/*
 * test_transport_public.c: conformance: exercise the candidate PUBLIC transport
 * API through the installed headers only.
 *
 * This TU deliberately includes ONLY <keel/...> headers, never a src/ internal header, to prove
 * the public surface (contract in <keel/{stream,listener,connect}.h>, layout in the matching
 * *_detail.h) is self-sufficient for an external transport author. It drives one representative
 * scenario per state machine through the public functions.
 */
#include "utest.h"
#include <keel/stream.h>
#include <keel/stream_detail.h>       /* embed/stack-allocate KlStream (opt-in detail) */
#include <keel/listener.h>
#include <keel/listener_detail.h>
#include <keel/connect_op.h>
#include <keel/connect_op_detail.h>
#include <keel/allocator.h>
#include <string.h>

/* ── KlConnectOp: resolve + connect entirely via the public API ────────────────────────────── */
typedef struct { KlConnectOp *op; int done, detached, last_result, last_fd; } COT;
static int  cot_resolve(void *c){ COT*m=c; kl_connect_op_on_resolved(m->op, 1); return 0; }
static int  cot_attempt(void *c, int idx, int *e){ (void)e; COT*m=c; kl_connect_op_on_attempt_connected(m->op, idx, (KlSocketHandle)(700+idx)); return 0; }
static void cot_dispose(void *c, KlSocketHandle fd){ (void)c; (void)fd; }
static void cot_done(void *c, KlConnectResult r, KlSocketHandle fd, int err){ (void)err; COT*m=c; m->done++; m->last_result=(int)r; m->last_fd=(int)fd; }
static void cot_detach(void *c){ COT*m=c; m->detached++; }

UTEST(transport_public, connect_op_success) {
    KlConnectOp op; COT m; memset(&m, 0, sizeof(m)); m.op = &op;
    KlConnectOpHooks h = {
        .start_resolve = cot_resolve, .start_attempt = cot_attempt,
        .dispose_fd = cot_dispose, .on_done = cot_done, .on_detach = cot_detach,
    };
    ASSERT_EQ(kl_connect_op_init(&op, &h, &m), 0);
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    ASSERT_EQ(m.done, 1);
    ASSERT_EQ(m.last_result, KL_CONNECT_SUCCESS);
    ASSERT_EQ(m.last_fd, 700);
    ASSERT_EQ(m.detached, 1);
    ASSERT_EQ(kl_connect_op_is_detached(&op), 1);
}

/* ── KlListener: reserve → accept → lease handoff via the public API ───────────────────────── */
typedef struct { KlListener *l; int slots; int accepts, releases, last_fd; KlSlotLease lease; } LTP;
static int  ltp_reserve(void *c){ LTP*m=c; if(m->slots<=0) return 0; m->slots--; return 1; }
static void ltp_release(void *c){ LTP*m=c; m->slots++; m->releases++; }
static int  ltp_arm(void *c){ (void)c; return 0; }
static void ltp_disarm(void *c){ (void)c; }
static void ltp_accept(void *c, KlSocketHandle fd, KlSlotLease lease){ LTP*m=c; m->accepts++; m->last_fd=(int)fd; m->lease=lease; }
static void ltp_dispose(void *c, KlSocketHandle fd){ (void)c; (void)fd; }

UTEST(transport_public, listener_accept_and_lease) {
    KlListener l; LTP m; memset(&m, 0, sizeof(m)); m.l = &l; m.slots = 2;
    KlListenerHooks h = {
        .reserve = ltp_reserve, .release = ltp_release, .credit_ctx = &m,
        .arm_accept = ltp_arm, .disarm_accept = ltp_disarm,
        .on_accept = ltp_accept, .dispose_fd = ltp_dispose,
    };
    ASSERT_EQ(kl_listener_init(&l, /*completion=*/0, &h, &m), 0);
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_STATE_LISTENING);

    kl_listener_on_accepted(&l, (KlSocketHandle)900);
    ASSERT_EQ(m.accepts, 1);
    ASSERT_EQ(m.last_fd, 900);

    kl_slot_lease_release(&m.lease);          /* accepted-stream owner releases its slot */
    ASSERT_EQ(m.releases, 1);
    kl_slot_lease_release(&m.lease);          /* consumed; no-op */
    ASSERT_EQ(m.releases, 1);

    kl_listener_close(&l);
    ASSERT_EQ(kl_listener_is_detached(&l), 1);
}

/* ── KlStream: write + read + close lifecycle via the public API ───────────────────────────── */
typedef struct { KlStream *s; int submits; int delivered; int closed; } STP;
static int  stp_submit(void *c, const char *d, size_t n){ (void)d; (void)n; STP*m=c; m->submits++; return 0; }
static void stp_deliver(void *c, const char *b, size_t n, int ok){ (void)b; (void)n; (void)ok; STP*m=c; m->delivered++; }
static int  stp_arm(void *c){ (void)c; return 0; }
static void stp_disarm(void *c){ (void)c; }
static void stp_close(void *c){ STP*m=c; m->closed++; }

UTEST(transport_public, stream_write_read_close) {
    KlAllocator a = kl_allocator_default();
    char buf[64];
    KlStream s;                                    /* stack storage (detail header = sizeof only) */
    STP m; memset(&m, 0, sizeof(m)); m.s = &s;

    ASSERT_EQ(kl_stream_init(&s, buf, sizeof(buf)), 0);   /* base init; no detail field access */
    ASSERT_EQ(kl_stream_write_init(&s, &a, 4096), 0);
    ASSERT_EQ(kl_stream_set_submit(&s, stp_submit, &m, 0), 0);
    ASSERT_EQ(kl_stream_read_init(&s, /*completion=*/1, stp_deliver, stp_arm, stp_disarm, &m), 0);
    ASSERT_EQ(kl_stream_close_init(&s, stp_close, &m), 0);

    ASSERT_EQ((int)kl_stream_write(&s, "hello", 5), KL_STREAM_ACCEPTED);
    ASSERT_EQ(m.submits, 1);
    ASSERT_EQ(kl_stream_read_start(&s), 0);
    memcpy(buf, "world", 5);
    ASSERT_EQ(kl_stream_on_recv(&s, 5, 1), 0);
    ASSERT_EQ(m.delivered, 1);

    ASSERT_EQ(kl_stream_close_begin(&s), 0);       /* graceful: waits for the in-flight send */
    ASSERT_EQ(m.closed, 0);
    ASSERT_EQ(kl_stream_on_write_complete(&s, 1), 0);
    /* recv still physically in flight (completion mode); retire it, then detach */
    ASSERT_EQ(kl_stream_on_recv(&s, 0, 0), 0);
    ASSERT_EQ(m.closed, 1);
    ASSERT_EQ(kl_stream_is_detached(&s), 1);
    ASSERT_EQ(kl_stream_write_free(&s), 0);
}

UTEST(transport_public, stream_init_validates_args) {
    char buf[16];
    KlStream s;
    ASSERT_EQ(kl_stream_init(NULL, buf, sizeof(buf)), -1);   /* NULL stream */
    ASSERT_EQ(kl_stream_init(&s, NULL, sizeof(buf)), -1);    /* NULL receive buffer */
    ASSERT_EQ(kl_stream_init(&s, buf, 0), -1);               /* zero capacity */
    ASSERT_EQ(kl_stream_init(&s, buf, sizeof(buf)), 0);      /* valid */
}

UTEST(transport_public, stream_reinit_after_detach_and_free) {
    /* The legal reinitialize path: a fully detached stream whose write queue has been freed may be
     * re-init'd for reuse (the only valid non-fresh precondition for kl_stream_init). */
    KlAllocator a = kl_allocator_default();
    char buf[64];
    KlStream s;
    STP m; memset(&m, 0, sizeof(m)); m.s = &s;

    ASSERT_EQ(kl_stream_init(&s, buf, sizeof(buf)), 0);
    ASSERT_EQ(kl_stream_write_init(&s, &a, 1024), 0);
    ASSERT_EQ(kl_stream_close_init(&s, stp_close, &m), 0);
    ASSERT_EQ(kl_stream_close_begin(&s), 0);        /* no ops outstanding → detaches at once */
    ASSERT_EQ(kl_stream_is_detached(&s), 1);
    ASSERT_EQ(kl_stream_write_free(&s), 0);         /* release provider-owned write queue */

    /* now legal to re-init the same storage and use it afresh */
    ASSERT_EQ(kl_stream_init(&s, buf, sizeof(buf)), 0);
    ASSERT_EQ(kl_stream_is_detached(&s), 0);        /* state fully reset */
    ASSERT_EQ(kl_stream_write_init(&s, &a, 1024), 0);
    ASSERT_EQ((int)kl_stream_write(&s, "x", 1), KL_STREAM_ERROR);  /* no writer/submit installed yet */
    ASSERT_EQ(kl_stream_write_free(&s), 0);
}

UTEST_MAIN();
