/*
 * completion_absent.c — the KEEL_NO_COMPLETION build's aborting stubs (RC-1).
 *
 * A readiness-only build that opts out of the completion axis entirely
 * (KEEL_NO_COMPLETION=1) still links the shared callers that REFERENCE the completion
 * entry points — async.c (kl_comp_run / kl_io_engine_resume_completion), server.c
 * (kl_io_engine_run_completion / kl_comp_cancel / kl_io_engine_post_read), udp.c
 * (kl_comp_post_udp_{recv,send}) — even though, on a readiness loop, those branches are
 * gated behind KL_EVENT_CAP_COMPLETION and never taken. This TU provides every such
 * symbol so the link resolves without pulling in the completion driver / dispatch /
 * backend. Reaching any of them means the completion gate was bypassed — a build/logic
 * error — so they abort() (fail-loud) rather than returning quietly.
 *
 * It REPLACES both the always-linked completion_dispatch.c (whose kl_comp_* dispatchers
 * are stubbed here) and completion_driver.c (the generic kl_comp_run / kl_io_engine_*),
 * selected by the Makefile (COMPLETION_CORE = src/completion_absent.c under
 * KEEL_NO_COMPLETION). No completion backend, driver, or dispatch is compiled in this
 * configuration; kl_comp_ops_builtin is not referenced (completion_dispatch.c is absent).
 *
 * The negotiation (kl_event_init_provider / kl_event_ctx_sockets_compatible in async.c)
 * rejects a KL_EVENT_CAP_COMPLETION provider under KEEL_NO_COMPLETION, so a completion
 * loop can never be installed and these stubs are unreachable in a correct program.
 */
#include "completion.h"
#include "io_engine.h"
#include <stdlib.h>   /* abort */

/* The completion axis is compiled OUT — the negotiation (async.c) reads this to reject a
 * KL_EVENT_CAP_COMPLETION provider before it can be installed (fail-loud). */
int kl_completion_axis_available(void) { return 0; }

/* ── kl_comp_* dispatchers (normally in completion_dispatch.c) ─────────────── */

int kl_comp_drain(struct KlEventCtx *ctx, KlCompletionEvent *out, int max, int timeout_ms) {
    (void)ctx; (void)out; (void)max; (void)timeout_ms;
    abort();   /* completion axis compiled out (KEEL_NO_COMPLETION) */
}

int kl_comp_prime_accepts(struct KlServer *s) { (void)s; abort(); }

int kl_comp_post_recv(KlConn *c) { (void)c; abort(); }

int kl_comp_post_send(KlConn *c, const KlIoVec *iov, int iovcnt, size_t total) {
    (void)c; (void)iov; (void)iovcnt; (void)total; abort();
}

int kl_comp_post_accept(struct KlServer *s) { (void)s; abort(); }

int kl_comp_post_sendfile(KlConn *c, const KlIoVec *head_iov, int head_n,
                          size_t head_total, int file_fd, uint64_t count) {
    (void)c; (void)head_iov; (void)head_n; (void)head_total; (void)file_fd; (void)count;
    abort();
}

void kl_comp_cancel(struct KlEventCtx *ctx, KlSocketHandle fd) { (void)ctx; (void)fd; abort(); }

int kl_comp_post_dgram_recv(struct KlEventCtx *ctx, const KlDgramRecvOp *op) { (void)ctx; (void)op; abort(); }

int kl_comp_post_dgram_send(struct KlEventCtx *ctx, const KlDgramSendOp *op) { (void)ctx; (void)op; abort(); }

int kl_comp_post_connect(struct KlEventCtx *ctx, KlSocketHandle fd,
                         const KlSockAddr *addr, void *watcher_udata) {
    (void)ctx; (void)fd; (void)addr; (void)watcher_udata; abort();
}

/* ── generic entry points (normally in completion_driver.c) ────────────────── */

int kl_comp_run(struct KlEventCtx *ctx, int max, int timeout_ms) {
    (void)ctx; (void)max; (void)timeout_ms; abort();
}

int kl_io_engine_run_completion(struct KlServer *s, int timeout_ms) {
    (void)s; (void)timeout_ms; abort();
}

int kl_io_engine_quiesce_accepts(struct KlServer *s) { (void)s; abort(); }

void kl_io_engine_resume_completion(struct KlServer *s, struct KlConn *conn) {
    (void)s; (void)conn; abort();
}

void kl_io_engine_post_read(struct KlConn *conn) { (void)conn; abort(); }
