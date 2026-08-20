/*
 * test_stream_read.c — Phase-B step 2B: strict read pause/resume machinery (src/stream_read.h).
 * Exercised in isolation with a mock deliver/arm/disarm and a stream whose read_buf is a plain
 * heap buffer (no live sockets). Covers the strict-pause invariants incl. reentrant pause/close
 * from the deliver callback and held terminal conditions.
 */
#include "utest.h"
#include <keel/http_connection.h>
#include <keel/allocator.h>
#include "../src/stream_read.h"
#include <string.h>
#include <stdlib.h>

/* ── Mock read hooks ─────────────────────────────────────────────────────── */
typedef struct {
    KlStream *s;
    int    delivered;             /* deliver-callback count */
    size_t last_len; int last_ok; char last[64];
    int    arm_calls; int arm_fail;
    int    disarm_calls;
    /* reentrant action performed INSIDE the deliver callback (once), to prove re-check: */
    int    pause_in_deliver;      /* call kl_stream_pause from the deliver callback */
    int    close_in_deliver;      /* call kl_stream_read_close from the deliver callback */
    int    start_in_deliver;      /* call kl_stream_read_start (terminal-restart attempt) */
    int    resume_in_deliver;     /* call kl_stream_resume from the deliver callback */
    /* synchronous arm completion: arm() calls on_recv inline this many more times: */
    int    sync_recv;             /* countdown of synchronous data completions from arm() */
    size_t sync_len;
    int    sync_then_fail;        /* arm() returns -1 AFTER a synchronous completion */
} RC;

static void rc_deliver(void *ctx, const char *buf, size_t len, int ok) {
    RC *r = ctx;
    r->delivered++;
    r->last_len = len; r->last_ok = ok;
    if (len && len <= sizeof(r->last)) memcpy(r->last, buf, len);
    if (r->pause_in_deliver)  { r->pause_in_deliver = 0;  kl_stream_pause(r->s); }
    if (r->close_in_deliver)  { r->close_in_deliver = 0;  kl_stream_read_close(r->s); }
    if (r->start_in_deliver)  { r->start_in_deliver = 0;  kl_stream_read_start(r->s); }
    if (r->resume_in_deliver) { r->resume_in_deliver = 0; kl_stream_resume(r->s); }
}
static int rc_arm(void *ctx) {
    RC *r = ctx; r->arm_calls++;
    if (r->arm_fail) return -1;
    if (r->sync_recv > 0) {          /* provider that completes the receive synchronously */
        r->sync_recv--;
        if (r->sync_len && r->sync_len <= r->s->read_cap) memcpy(r->s->read_buf, "SYNC", 4);
        kl_stream_on_recv(r->s, r->sync_len, 1);   /* inline completion */
        if (r->sync_then_fail) return -1;          /* hook reports failure AFTER completing inline */
    }
    return 0;
}
static void rc_disarm(void *ctx) { RC *r = ctx; r->disarm_calls++; }

/* Build a KlStream with a heap read_buf of `cap` bytes. */
static void mk_stream(KlStream *s, char **buf, size_t cap) {
    memset(s, 0, sizeof(*s));
    *buf = malloc(cap);
    s->read_buf = *buf;
    s->read_cap = cap;
}
static void put(KlStream *s, const char *data, size_t len) { memcpy(s->read_buf, data, len); }

/* ── Tests ───────────────────────────────────────────────────────────────── */

UTEST(stream_read, active_delivers_then_rearms) {
    KlStream s; char *buf; mk_stream(&s, &buf, 256);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s;
    ASSERT_EQ(kl_stream_read_init(&s, /*completion=*/1, rc_deliver, rc_arm, rc_disarm, &r), 0);

    ASSERT_EQ(kl_stream_read_start(&s), 0);
    ASSERT_EQ(r.arm_calls, 1);                       /* first receive armed */
    put(&s, "hello", 5);
    ASSERT_EQ(kl_stream_on_recv(&s, 5, 1), 0);
    ASSERT_EQ(r.delivered, 1);
    ASSERT_EQ((int)r.last_len, 5);
    ASSERT_EQ(memcmp(r.last, "hello", 5), 0);
    ASSERT_EQ(r.arm_calls, 2);                       /* re-armed after delivery */

    free(buf);
}

UTEST(stream_read, completion_pause_holds_inflight_then_resume_delivers_once) {
    KlStream s; char *buf; mk_stream(&s, &buf, 256);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s;
    ASSERT_EQ(kl_stream_read_init(&s, /*completion=*/1, rc_deliver, rc_arm, rc_disarm, &r), 0);
    ASSERT_EQ(kl_stream_read_start(&s), 0);          /* recv posted (in flight) */

    kl_stream_pause(&s);
    ASSERT_EQ(r.disarm_calls, 0);                    /* completion pause does NOT disarm */

    put(&s, "world", 5);
    ASSERT_EQ(kl_stream_on_recv(&s, 5, 1), 0);       /* completes while paused → HELD */
    ASSERT_EQ(r.delivered, 0);
    ASSERT_EQ(kl_stream_read_held(&s), 1);

    ASSERT_EQ(kl_stream_resume(&s), 0);              /* deliver held exactly once, then re-arm */
    ASSERT_EQ(r.delivered, 1);
    ASSERT_EQ((int)r.last_len, 5);
    ASSERT_EQ(kl_stream_read_held(&s), 0);
    ASSERT_EQ(r.arm_calls, 2);
    ASSERT_EQ(kl_stream_resume(&s), 0);              /* idempotent — not paused, no re-deliver */
    ASSERT_EQ(r.delivered, 1);

    free(buf);
}

UTEST(stream_read, readiness_pause_disarms_and_clears_inflight) {
    KlStream s; char *buf; mk_stream(&s, &buf, 256);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s;
    ASSERT_EQ(kl_stream_read_init(&s, /*completion=*/0, rc_deliver, rc_arm, rc_disarm, &r), 0);
    ASSERT_EQ(kl_stream_read_start(&s), 0);
    ASSERT_EQ(r.arm_calls, 1);

    kl_stream_pause(&s);
    ASSERT_EQ(r.disarm_calls, 1);                    /* readiness drops READ interest */
    kl_stream_pause(&s);                             /* idempotent */
    ASSERT_EQ(r.disarm_calls, 1);

    ASSERT_EQ(kl_stream_resume(&s), 0);              /* no held → re-arm interest */
    ASSERT_EQ(r.arm_calls, 2);
    ASSERT_EQ(r.delivered, 0);

    free(buf);
}

UTEST(stream_read, terminal_while_paused_held_then_closes_once) {
    KlStream s; char *buf; mk_stream(&s, &buf, 256);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s;
    ASSERT_EQ(kl_stream_read_init(&s, 1, rc_deliver, rc_arm, rc_disarm, &r), 0);
    ASSERT_EQ(kl_stream_read_start(&s), 0);
    kl_stream_pause(&s);

    ASSERT_EQ(kl_stream_on_recv(&s, 0, /*ok=*/0), 0); /* EOF/error while paused → held terminal */
    ASSERT_EQ(kl_stream_read_held(&s), 1);
    ASSERT_EQ(r.delivered, 0);

    int arm_before = r.arm_calls;
    ASSERT_EQ(kl_stream_resume(&s), 0);
    ASSERT_EQ(r.delivered, 1);                        /* terminal delivered exactly once */
    ASSERT_EQ(r.last_ok, 0);
    ASSERT_EQ(r.arm_calls, arm_before);              /* terminal → NOT re-armed */
    ASSERT_EQ(kl_stream_resume(&s), 0);              /* idempotent */
    ASSERT_EQ(r.delivered, 1);

    free(buf);
}

UTEST(stream_read, repeated_pause_resume_before_completion) {
    KlStream s; char *buf; mk_stream(&s, &buf, 256);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s;
    ASSERT_EQ(kl_stream_read_init(&s, 1, rc_deliver, rc_arm, rc_disarm, &r), 0);
    ASSERT_EQ(kl_stream_read_start(&s), 0);          /* one recv in flight (arm=1) */

    kl_stream_pause(&s); ASSERT_EQ(kl_stream_resume(&s), 0);
    kl_stream_pause(&s); ASSERT_EQ(kl_stream_resume(&s), 0);
    ASSERT_EQ(r.delivered, 0);                        /* nothing completed yet */
    ASSERT_EQ(r.arm_calls, 1);                        /* recv still in flight — no spurious re-arm */

    put(&s, "data", 4);
    ASSERT_EQ(kl_stream_on_recv(&s, 4, 1), 0);
    ASSERT_EQ(r.delivered, 1);

    free(buf);
}

UTEST(stream_read, pause_inside_held_deliver_suppresses_rearm) {
    KlStream s; char *buf; mk_stream(&s, &buf, 256);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s;
    ASSERT_EQ(kl_stream_read_init(&s, 1, rc_deliver, rc_arm, rc_disarm, &r), 0);
    ASSERT_EQ(kl_stream_read_start(&s), 0);
    kl_stream_pause(&s);
    put(&s, "held!", 5);
    ASSERT_EQ(kl_stream_on_recv(&s, 5, 1), 0);       /* held */

    r.pause_in_deliver = 1;                          /* the held-data callback pauses again */
    int arm_before = r.arm_calls;
    ASSERT_EQ(kl_stream_resume(&s), 0);
    ASSERT_EQ(r.delivered, 1);                        /* delivered once */
    ASSERT_EQ(r.arm_calls, arm_before);              /* re-check saw pause → did NOT re-arm */
    ASSERT_EQ(kl_stream_read_held(&s), 0);           /* held consumed */

    free(buf);
}

UTEST(stream_read, close_inside_active_deliver_suppresses_rearm) {
    /* Invariant 6: delivery synchronously closing the stream must not re-arm. */
    KlStream s; char *buf; mk_stream(&s, &buf, 256);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s;
    ASSERT_EQ(kl_stream_read_init(&s, 1, rc_deliver, rc_arm, rc_disarm, &r), 0);
    ASSERT_EQ(kl_stream_read_start(&s), 0);          /* arm=1 */
    r.close_in_deliver = 1;
    put(&s, "bye", 3);
    ASSERT_EQ(kl_stream_on_recv(&s, 3, 1), 0);
    ASSERT_EQ(r.delivered, 1);
    ASSERT_EQ(r.arm_calls, 1);                        /* closed in callback → no re-arm */

    free(buf);
}

UTEST(stream_read, close_while_held_discards_without_delivery) {
    KlStream s; char *buf; mk_stream(&s, &buf, 256);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s;
    ASSERT_EQ(kl_stream_read_init(&s, 1, rc_deliver, rc_arm, rc_disarm, &r), 0);
    ASSERT_EQ(kl_stream_read_start(&s), 0);
    kl_stream_pause(&s);
    put(&s, "gone", 4);
    ASSERT_EQ(kl_stream_on_recv(&s, 4, 1), 0);       /* held */
    ASSERT_EQ(kl_stream_read_held(&s), 1);

    kl_stream_read_close(&s);                        /* close while held */
    ASSERT_EQ(kl_stream_read_held(&s), 0);           /* held discarded */
    int arm_before = r.arm_calls;
    ASSERT_EQ(kl_stream_resume(&s), 0);              /* closed → nothing delivered / re-armed */
    ASSERT_EQ(r.delivered, 0);
    ASSERT_EQ(r.arm_calls, arm_before);

    free(buf);
}

UTEST(stream_read, recv_at_exact_capacity_delivers) {
    KlStream s; char *buf; mk_stream(&s, &buf, 8);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s;
    ASSERT_EQ(kl_stream_read_init(&s, 1, rc_deliver, rc_arm, rc_disarm, &r), 0);
    ASSERT_EQ(kl_stream_read_start(&s), 0);
    ASSERT_EQ(kl_stream_on_recv(&s, 8, 1), 0);        /* exactly read_cap — fine */
    ASSERT_EQ(r.delivered, 1);
    ASSERT_EQ((int)r.last_len, 8);
    free(buf);
}

UTEST(stream_read, oversized_recv_fails_closed) {
    /* Finding 5: len > read_cap is a backend contract violation — fail closed, never deliver. */
    KlStream s; char *buf; mk_stream(&s, &buf, 8);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s;
    ASSERT_EQ(kl_stream_read_init(&s, 1, rc_deliver, rc_arm, rc_disarm, &r), 0);
    ASSERT_EQ(kl_stream_read_start(&s), 0);
    ASSERT_EQ(kl_stream_on_recv(&s, 9, 1), -1);       /* read_cap + 1 → error */
    ASSERT_EQ(r.delivered, 0);                         /* not delivered as data */
    ASSERT_EQ(s.read_closed, 1);
    free(buf);
}

UTEST(stream_read, sync_arm_completion_start_resume_and_rearm) {
    /* Finding 1: arm() may complete synchronously (calls on_recv inline). recv_inflight is set
     * before arm and not clobbered afterward; delivery + re-arm happen correctly. */
    KlStream s; char *buf; mk_stream(&s, &buf, 64);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s; r.sync_len = 4;
    ASSERT_EQ(kl_stream_read_init(&s, /*completion=*/1, rc_deliver, rc_arm, rc_disarm, &r), 0);

    /* start(): arm completes synchronously → delivers, then re-arms (which posts normally). */
    r.sync_recv = 1;
    ASSERT_EQ(kl_stream_read_start(&s), 0);
    ASSERT_EQ(r.delivered, 1);                         /* the synchronous completion was delivered */
    ASSERT_EQ(s.recv_inflight, 1);                     /* the post-delivery re-arm is in flight */
    ASSERT_EQ(r.arm_calls, 2);

    /* A normal completion of that armed receive delivers again. */
    put(&s, "data", 4);
    ASSERT_EQ(kl_stream_on_recv(&s, 4, 1), 0);
    ASSERT_EQ(r.delivered, 2);
    free(buf);
}

UTEST(stream_read, sync_completion_then_hook_returns_error) {
    /* Finding 1: a synchronous completion RETIRES that arm; a subsequent read_arm() return of -1
     * must NOT retroactively fail/close it. */
    KlStream s; char *buf; mk_stream(&s, &buf, 64);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s; r.sync_len = 4;
    ASSERT_EQ(kl_stream_read_init(&s, 1, rc_deliver, rc_arm, rc_disarm, &r), 0);
    r.sync_recv = 1; r.sync_then_fail = 1;
    ASSERT_EQ(kl_stream_read_start(&s), 0);            /* NOT -1: the inline completion won */
    ASSERT_EQ(r.delivered, 1);
    ASSERT_EQ(s.read_closed, 0);                       /* stale -1 did not close the stream */
    /* The post-delivery re-arm (2nd arm) posts normally (sync_recv exhausted). */
    ASSERT_EQ(s.recv_inflight, 1);
    free(buf);
}

UTEST(stream_read, sync_completion_reaches_second_arm) {
    /* Two synchronous completions in a row: delivered twice via the iterative trampoline. */
    KlStream s; char *buf; mk_stream(&s, &buf, 64);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s; r.sync_len = 4;
    ASSERT_EQ(kl_stream_read_init(&s, 1, rc_deliver, rc_arm, rc_disarm, &r), 0);
    r.sync_recv = 2;
    ASSERT_EQ(kl_stream_read_start(&s), 0);
    ASSERT_EQ(r.delivered, 2);                         /* both inline completions delivered */
    ASSERT_EQ(s.recv_inflight, 1);                     /* third arm posted async */
    free(buf);
}

UTEST(stream_read, many_sync_completions_bounded_stack) {
    /* Many consecutive synchronous completions must not recurse (bounded C stack): the trampoline
     * loops iteratively. A count that would blow a recursive stack completes fine here. */
    KlStream s; char *buf; mk_stream(&s, &buf, 64);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s; r.sync_len = 4;
    ASSERT_EQ(kl_stream_read_init(&s, 1, rc_deliver, rc_arm, rc_disarm, &r), 0);
    r.sync_recv = 200000;
    ASSERT_EQ(kl_stream_read_start(&s), 0);
    ASSERT_EQ(r.delivered, 200000);
    free(buf);
}

UTEST(stream_read, pause_during_sync_delivery_suppresses_rearm) {
    /* A synchronous completion whose deliver callback pauses must not re-arm afterward. */
    KlStream s; char *buf; mk_stream(&s, &buf, 64);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s; r.sync_len = 4;
    ASSERT_EQ(kl_stream_read_init(&s, 1, rc_deliver, rc_arm, rc_disarm, &r), 0);
    r.sync_recv = 5;                 /* would keep completing, but the pause stops after the first */
    r.pause_in_deliver = 1;
    ASSERT_EQ(kl_stream_read_start(&s), 0);
    ASSERT_EQ(r.delivered, 1);                         /* delivered once, then paused */
    ASSERT_EQ(s.recv_inflight, 0);                     /* no deferred re-arm */
    ASSERT_EQ(s.read_paused, 1);
    ASSERT_EQ(r.arm_calls, 1);                         /* trampoline did not arm again */
    free(buf);
}

UTEST(stream_read, close_during_sync_delivery_suppresses_rearm) {
    KlStream s; char *buf; mk_stream(&s, &buf, 64);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s; r.sync_len = 4;
    ASSERT_EQ(kl_stream_read_init(&s, 1, rc_deliver, rc_arm, rc_disarm, &r), 0);
    r.sync_recv = 5;
    r.close_in_deliver = 1;
    ASSERT_EQ(kl_stream_read_start(&s), 0);
    ASSERT_EQ(r.delivered, 1);
    ASSERT_EQ(s.read_closed, 1);
    ASSERT_EQ(r.arm_calls, 1);                         /* no re-arm after close */
    free(buf);
}

UTEST(stream_read, double_close_is_idempotent) {
    /* Finding 2: repeated close must not re-invoke disarm. */
    KlStream s; char *buf; mk_stream(&s, &buf, 64);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s;
    ASSERT_EQ(kl_stream_read_init(&s, /*completion=*/0, rc_deliver, rc_arm, rc_disarm, &r), 0);
    ASSERT_EQ(kl_stream_read_start(&s), 0);
    kl_stream_read_close(&s);
    ASSERT_EQ(r.disarm_calls, 1);
    kl_stream_read_close(&s);                          /* idempotent — no second disarm */
    ASSERT_EQ(r.disarm_calls, 1);

    /* Completion mode double close is also a no-op after the first. */
    KlStream s2; char *buf2; mk_stream(&s2, &buf2, 64);
    RC r2; memset(&r2, 0, sizeof(r2)); r2.s = &s2;
    ASSERT_EQ(kl_stream_read_init(&s2, 1, rc_deliver, rc_arm, rc_disarm, &r2), 0);
    ASSERT_EQ(kl_stream_read_start(&s2), 0);
    kl_stream_read_close(&s2);
    kl_stream_read_close(&s2);
    ASSERT_EQ(s2.read_closed, 1);
    free(buf); free(buf2);
}

UTEST(stream_read, pause_before_start_does_not_disarm) {
    /* Readiness pause with no armed interest must not call disarm (nothing to drop). */
    KlStream s; char *buf; mk_stream(&s, &buf, 64);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s;
    ASSERT_EQ(kl_stream_read_init(&s, /*completion=*/0, rc_deliver, rc_arm, rc_disarm, &r), 0);
    kl_stream_pause(&s);                               /* before any start() */
    ASSERT_EQ(r.disarm_calls, 0);                      /* no nonexistent interest disarmed */
    ASSERT_EQ(s.read_paused, 1);
    free(buf);
}

UTEST(stream_read, sync_arm_completion_on_resume_readiness) {
    /* resume() with a synchronous-completing arm and no held data. Readiness mode so pause
     * clears the pending interest → resume arms afresh, which completes inline and delivers. */
    KlStream s; char *buf; mk_stream(&s, &buf, 64);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s; r.sync_len = 4;
    ASSERT_EQ(kl_stream_read_init(&s, /*completion=*/0, rc_deliver, rc_arm, rc_disarm, &r), 0);
    ASSERT_EQ(kl_stream_read_start(&s), 0);            /* arm=1, inflight=1 */
    kl_stream_pause(&s);                               /* readiness: disarm → inflight=0 */
    ASSERT_EQ(s.recv_inflight, 0);

    r.sync_recv = 1;
    ASSERT_EQ(kl_stream_resume(&s), 0);               /* no held → arm → sync completes → deliver */
    ASSERT_EQ(r.delivered, 1);
    ASSERT_EQ(s.recv_inflight, 1);                     /* re-armed after the sync delivery */
    free(buf);
}

UTEST(stream_read, completion_close_leaves_inflight_until_retirement) {
    /* Finding 2: logical close must not falsely retire a physically-outstanding completion recv. */
    KlStream s; char *buf; mk_stream(&s, &buf, 64);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s;
    ASSERT_EQ(kl_stream_read_init(&s, /*completion=*/1, rc_deliver, rc_arm, rc_disarm, &r), 0);
    ASSERT_EQ(kl_stream_read_start(&s), 0);           /* recv physically in flight */
    ASSERT_EQ(s.recv_inflight, 1);

    kl_stream_read_close(&s);                          /* logical close */
    ASSERT_EQ(s.read_closed, 1);
    ASSERT_EQ(s.recv_inflight, 1);                     /* provider op STILL outstanding */

    /* The provider's completion arrives = retirement: dropped (not delivered), inflight cleared. */
    ASSERT_EQ(kl_stream_on_recv(&s, 5, 1), 0);
    ASSERT_EQ(r.delivered, 0);
    ASSERT_EQ(s.recv_inflight, 0);                     /* now retired */
    free(buf);
}

UTEST(stream_read, readiness_close_clears_inflight) {
    KlStream s; char *buf; mk_stream(&s, &buf, 64);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s;
    ASSERT_EQ(kl_stream_read_init(&s, /*completion=*/0, rc_deliver, rc_arm, rc_disarm, &r), 0);
    ASSERT_EQ(kl_stream_read_start(&s), 0);
    kl_stream_read_close(&s);
    ASSERT_EQ(s.recv_inflight, 0);                     /* readiness: interest dropped, nothing outstanding */
    ASSERT_EQ(r.disarm_calls, 1);
    free(buf);
}

UTEST(stream_read, duplicate_completion_dropped) {
    /* Finding 3: a completion with recv_inflight already clear must be dropped, never delivered. */
    KlStream s; char *buf; mk_stream(&s, &buf, 64);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s;
    ASSERT_EQ(kl_stream_read_init(&s, 1, rc_deliver, rc_arm, rc_disarm, &r), 0);
    ASSERT_EQ(kl_stream_read_start(&s), 0);
    put(&s, "one", 3);
    ASSERT_EQ(kl_stream_on_recv(&s, 3, 1), 0);        /* delivered; re-armed */
    ASSERT_EQ(r.delivered, 1);
    int inflight = s.recv_inflight;

    /* A stray SECOND completion for the already-retired op: pretend nothing is in flight. */
    s.recv_inflight = 0;
    ASSERT_EQ(kl_stream_on_recv(&s, 3, 1), 0);        /* dropped */
    ASSERT_EQ(r.delivered, 1);                         /* not delivered again */
    s.recv_inflight = inflight;                        /* restore for a clean free path */
    free(buf);
}

UTEST(stream_read, duplicate_completion_while_held_preserves_held) {
    KlStream s; char *buf; mk_stream(&s, &buf, 64);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s;
    ASSERT_EQ(kl_stream_read_init(&s, 1, rc_deliver, rc_arm, rc_disarm, &r), 0);
    ASSERT_EQ(kl_stream_read_start(&s), 0);
    kl_stream_pause(&s);
    put(&s, "held", 4);
    ASSERT_EQ(kl_stream_on_recv(&s, 4, 1), 0);        /* held (recv_inflight now 0) */
    ASSERT_EQ(kl_stream_read_held(&s), 1);
    ASSERT_EQ((int)s.held_len, 4);

    ASSERT_EQ(kl_stream_on_recv(&s, 99, 1), 0);       /* spurious dup (inflight clear) — dropped */
    ASSERT_EQ(kl_stream_read_held(&s), 1);            /* held completion NOT replaced */
    ASSERT_EQ((int)s.held_len, 4);
    ASSERT_EQ(kl_stream_resume(&s), 0);
    ASSERT_EQ((int)r.last_len, 4);                     /* original held bytes delivered */
    free(buf);
}

UTEST(stream_read, terminal_callback_cannot_restart) {
    /* Finding 4: terminal state is set BEFORE the callback, so a terminal callback calling
     * start()/resume() cannot re-arm a closed stream. */
    KlStream s; char *buf; mk_stream(&s, &buf, 64);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s;
    ASSERT_EQ(kl_stream_read_init(&s, 1, rc_deliver, rc_arm, rc_disarm, &r), 0);
    ASSERT_EQ(kl_stream_read_start(&s), 0);
    int arm_before = r.arm_calls;
    r.start_in_deliver = 1;                            /* terminal callback tries to restart */
    ASSERT_EQ(kl_stream_on_recv(&s, 0, /*ok=*/0), 0); /* terminal */
    ASSERT_EQ(r.delivered, 1);
    ASSERT_EQ(r.last_ok, 0);
    ASSERT_EQ(r.last_len, (size_t)0);                  /* terminal carries no data */
    ASSERT_EQ(r.arm_calls, arm_before);               /* start() inside terminal did NOT re-arm */
    ASSERT_EQ(s.read_closed, 1);
    free(buf);
}

UTEST(stream_read, init_validates_buffer_and_readiness_disarm) {
    KlAllocator a = kl_allocator_default(); (void)a;
    RC r; memset(&r, 0, sizeof(r));
    KlStream s; memset(&s, 0, sizeof(s));

    /* No read buffer → rejected. */
    ASSERT_EQ(kl_stream_read_init(&s, 1, rc_deliver, rc_arm, rc_disarm, &r), -1);

    /* Readiness mode requires a disarm hook. */
    char *buf; mk_stream(&s, &buf, 64);
    ASSERT_EQ(kl_stream_read_init(&s, /*completion=*/0, rc_deliver, rc_arm, /*disarm=*/NULL, &r), -1);
    /* Completion mode may omit disarm. */
    ASSERT_EQ(kl_stream_read_init(&s, /*completion=*/1, rc_deliver, rc_arm, NULL, &r), 0);
    free(buf);
}

UTEST(stream_read, arm_failure_closes_read_side) {
    KlStream s; char *buf; mk_stream(&s, &buf, 64);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s; r.arm_fail = 1;
    ASSERT_EQ(kl_stream_read_init(&s, 1, rc_deliver, rc_arm, rc_disarm, &r), 0);
    ASSERT_EQ(kl_stream_read_start(&s), -1);          /* arm failed */
    ASSERT_EQ(s.read_closed, 1);
    free(buf);
}

UTEST(stream_read, init_rejects_double) {
    KlStream s; char *buf; mk_stream(&s, &buf, 64);
    RC r; memset(&r, 0, sizeof(r)); r.s = &s;
    ASSERT_EQ(kl_stream_read_init(&s, 1, rc_deliver, rc_arm, rc_disarm, &r), 0);
    ASSERT_EQ(kl_stream_read_init(&s, 1, rc_deliver, rc_arm, rc_disarm, &r), -1);
    free(buf);
}

UTEST_MAIN();
