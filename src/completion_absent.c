/*
 * completion_absent.c — the KEEL_NO_COMPLETION build's NEUTRAL aborting stubs (RC-1; split in R2f).
 *
 * A readiness-only build that opts out of the completion axis entirely
 * (KEEL_NO_COMPLETION=1) still links the shared callers that REFERENCE the neutral completion
 * entry points — async.c (kl_comp_run via kl_event_ctx_run), http_server_core.c (kl_comp_cancel),
 * datagram.c (kl_comp_post_dgram_{recv,send} / cancel_dgram / retire_dgram), the async client
 * (kl_comp_post_connect) — even though, on a readiness loop, those branches are gated behind
 * KL_EVENT_CAP_COMPLETION and never taken. This TU provides every such NEUTRAL symbol so the link
 * resolves without pulling in the completion driver / dispatch / backend. Reaching any of them means
 * the completion gate was bypassed — a build/logic error — so they abort() (fail-loud).
 *
 * It REPLACES the always-linked substrate completion TUs — completion_dispatch.c (the neutral
 * kl_comp_*_raw dispatchers + kl_completion_axis_available) and completion_core.c (kl_comp_run) —
 * selected by the Makefile (COMPLETION_CORE = src/completion_absent.c under KEEL_NO_COMPLETION). The
 * HTTP-typed completion surface (the KlHttpServer/KlHttpConn wrappers + the kl_http_comp_* run-loop
 * orchestration) is stubbed separately in src/protocols/http/completion_http_absent.c, mirroring the hosted
 * split (substrate dispatch vs completion_http_server.c) so this substrate TU names NO HTTP type (R2f;
 * docs/protocols_restructure_freeze.md §4.8). No completion backend, driver, or dispatch is compiled in
 * this configuration; kl_comp_ops_builtin is not referenced (completion_dispatch.c is absent).
 *
 * The negotiation (kl_event_init_provider / kl_event_ctx_sockets_compatible in async.c)
 * rejects a KL_EVENT_CAP_COMPLETION provider under KEEL_NO_COMPLETION, so a completion
 * loop can never be installed and these stubs are unreachable in a correct program.
 */
#include "completion.h"
#include "completion_io.h"
#include <stdlib.h>   /* abort */

/* The completion axis is compiled OUT — the negotiation (async.c) reads this to reject a
 * KL_EVENT_CAP_COMPLETION provider before it can be installed (fail-loud). */
int kl_completion_axis_available(void) { return 0; }

/* ── Neutral kl_comp_*_raw dispatchers (normally in completion_dispatch.c) ─────────────── */

int kl_comp_drain(struct KlEventCtx *ctx, KlCompletionEvent *out, int max, int timeout_ms) {
    (void)ctx; (void)out; (void)max; (void)timeout_ms;
    abort();   /* completion axis compiled out (KEEL_NO_COMPLETION) */
}

int kl_comp_prime_accepts_raw(struct KlEventCtx *ctx, KlSocketHandle listen_fd) {
    (void)ctx; (void)listen_fd; abort();
}

int kl_comp_shutdown_accepts_raw(struct KlEventCtx *ctx) { (void)ctx; abort(); }

int kl_comp_post_recv_raw(KlStream *stream, void *buf, size_t cap) {
    (void)stream; (void)buf; (void)cap; abort();
}

int kl_comp_post_send_raw(KlStream *stream, const KlIoVec *iov, int iovcnt, size_t total) {
    (void)stream; (void)iov; (void)iovcnt; (void)total; abort();
}

int kl_comp_post_accept_raw(struct KlEventCtx *ctx) { (void)ctx; abort(); }

int kl_comp_post_sendfile_raw(KlStream *stream, const KlIoVec *head_iov, int head_n,
                              size_t head_total, int file_fd, uint64_t count) {
    (void)stream; (void)head_iov; (void)head_n; (void)head_total; (void)file_fd; (void)count;
    abort();
}

void kl_comp_cancel(struct KlEventCtx *ctx, KlSocketHandle fd) { (void)ctx; (void)fd; abort(); }

int kl_comp_post_dgram_recv(struct KlEventCtx *ctx, const KlDgramRecvOp *op) { (void)ctx; (void)op; abort(); }

int kl_comp_post_dgram_send(struct KlEventCtx *ctx, const KlDgramSendOp *op) { (void)ctx; (void)op; abort(); }

int kl_comp_cancel_dgram(struct KlEventCtx *ctx, struct KlDgramLife *life, KlDgramOpKind kind) {
    (void)ctx; (void)life; (void)kind; abort();
}

KlDgramRetireResult kl_comp_retire_dgram(struct KlEventCtx *ctx, struct KlDgramLife *life,
                                         KlDgramOpKind kind, int *transport_err) {
    (void)ctx; (void)life; (void)kind; (void)transport_err; abort();
}

int kl_comp_post_connect(struct KlEventCtx *ctx, KlSocketHandle fd,
                         const KlSockAddr *addr, void *watcher_udata) {
    (void)ctx; (void)fd; (void)addr; (void)watcher_udata; abort();
}

/* ── generic tick (normally in completion_core.c) ────────────────── */

int kl_comp_run(struct KlEventCtx *ctx, int max, int timeout_ms) {
    (void)ctx; (void)max; (void)timeout_ms; abort();
}
