/*
 * server_stats_probe.h: read kl_http_server_stats of a RUNNING server from the test thread.
 *
 * kl_http_server_stats is a loop-thread read (it walks the suspended-operation list), so a test
 * thread must not call it while the server runs on another thread. The probe asks the loop for
 * the snapshot instead: a KlWakeup whose watcher, on the loop thread, takes the stats and
 * publishes them through seq-cst atomics; the test thread signals the wakeup and waits for the
 * publication.
 *
 *   KlTestStatsProbe probe;
 *   kl_test_stats_probe_init(&probe, &srv);      // after kl_http_server_init, BEFORE the run thread
 *   ... start the thread running kl_http_server_run(&srv) ...
 *   KlHttpServerStats st;
 *   kl_test_stats_probe_get(&probe, &st);         // any time while the server runs
 *   ... stop + join ...
 *   kl_test_stats_probe_free(&probe);             // after the join, before kl_http_server_free
 *
 * Header-only (static inline); each suite that includes it gets its own copy.
 */
#ifndef KEEL_TESTS_SERVER_STATS_PROBE_H
#define KEEL_TESTS_SERVER_STATS_PROBE_H

#include <keel/event_ctx.h>
#include <keel/http_server.h>
#include <keel/wakeup.h>
#include "kl_atomic.h"
#include "net_compat.h"   /* kl_test_sleep_ms */

typedef struct {
    KlHttpServer *srv;
    KlWakeup      wk;
    int           seq;                          /* bumped (atomically) after each publication */
    int           active, max, suspended, paused;   /* the published snapshot (atomic ints) */
    /* Optional: run on the loop thread with each snapshot, before it is published, for a suite
     * that needs more loop-owned state than the stats (publish it through atomics too). */
    void        (*on_loop)(KlHttpServer *srv, void *ud);
    void         *on_loop_ud;
} KlTestStatsProbe;

static inline void kl_test_stats_probe_on_wake(KlSocketHandle fd, KlEventMask ready, void *ud) {
    (void)fd; (void)ready;
    KlTestStatsProbe *p = (KlTestStatsProbe *)ud;
    kl_wakeup_drain(&p->wk);
    KlHttpServerStats st;
    kl_http_server_stats(p->srv, &st);          /* on the loop thread */
    if (p->on_loop) p->on_loop(p->srv, p->on_loop_ud);
    kl_atomic_store_int(&p->active, st.active_connections);
    kl_atomic_store_int(&p->max, st.max_connections);
    kl_atomic_store_int(&p->suspended, st.async_suspended);
    kl_atomic_store_int(&p->paused, st.listen_paused);
    kl_atomic_store_int(&p->seq, kl_atomic_load_int(&p->seq) + 1);   /* only this thread writes */
}

/* Register the probe on @p srv's loop. Call before the thread running the server is created. */
static inline int kl_test_stats_probe_init(KlTestStatsProbe *p, KlHttpServer *srv) {
    p->srv = srv;
    p->seq = 0;
    p->active = p->max = p->suspended = p->paused = 0;
    p->on_loop = NULL;
    p->on_loop_ud = NULL;
    if (kl_wakeup_open(&p->wk) != 0) return -1;
    if (kl_watcher_add(kl_http_server_event_ctx(srv), p->wk.rd, KL_EVENT_READ,
                       kl_test_stats_probe_on_wake, p) != 0) {
        kl_wakeup_close(&p->wk);
        return -1;
    }
    return 0;
}

/* Snapshot taken on the loop thread. Returns 0, or -1 (zeroed *out) if the loop did not answer
 * within about two seconds (the server is not running). One test thread per probe. */
static inline int kl_test_stats_probe_get(KlTestStatsProbe *p, KlHttpServerStats *out) {
    int before = kl_atomic_load_int(&p->seq);
    kl_wakeup_signal(&p->wk);
    for (int i = 0; i < 2000; i++) {
        if (kl_atomic_load_int(&p->seq) != before) {
            out->active_connections = kl_atomic_load_int(&p->active);
            out->max_connections    = kl_atomic_load_int(&p->max);
            out->async_suspended    = kl_atomic_load_int(&p->suspended);
            out->listen_paused      = kl_atomic_load_int(&p->paused);
            return 0;
        }
        kl_test_sleep_ms(1);
    }
    out->active_connections = out->max_connections = out->async_suspended = out->listen_paused = 0;
    return -1;
}

/* Unregister and close. Call once the server's run has returned (thread joined). */
static inline void kl_test_stats_probe_free(KlTestStatsProbe *p) {
    kl_watcher_del(kl_http_server_event_ctx(p->srv), p->wk.rd);
    kl_wakeup_close(&p->wk);
}

#endif /* KEEL_TESTS_SERVER_STATS_PROBE_H */
