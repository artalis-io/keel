/*
 * completion_dispatch.c — the kl_comp_* completion-primitive call surface, dispatched
 * to either the compiled-in backend (default, no indirection) or a runtime-installed
 * provider. The completion-axis counterpart of event_dispatch.c (RC-1).
 *
 * When loop->ops is NULL (the common case) each call goes straight to the backend's
 * completion vtable via kl_comp_ops_builtin() — the same table the backend also hangs
 * off its KlEventOps.completion. When a bring-your-own event backend is installed
 * (KlEventCtx.event_provider → loop->ops), the same calls route through
 * loop->ops->completion instead, so the completion axis is runtime-injectable without a
 * core recompile. RC-1 relocates the primitives behind this dispatch WITHOUT changing
 * behavior: the default (loop->ops == NULL) path reaches the identical backend impls.
 *
 * These definitions OWN the public kl_comp_* names; each completion backend renames its
 * own impls (static) and exposes them through kl_comp_ops_builtin(). The generic driver
 * (completion_driver.c) and the completion callers (async.c / server.c) call
 * these free functions unchanged. See completion.h / event_dispatch.c.
 */
#include <keel/event_ctx.h>   /* KlEventCtx (->loop), KlHttpServer/KlHttpConn reach the loop */
#include <keel/http_server.h>      /* struct KlHttpServer (->ev.loop) */
#include <keel/http_connection.h>  /* struct KlHttpConn (->ctx->loop) */
#include "completion.h"
#include "io_engine.h"        /* kl_completion_axis_available */

/* The completion axis IS compiled in this build (completion_absent.c returns 0). */
int kl_completion_axis_available(void) { return 1; }

/* Resolve the completion vtable for a loop: the runtime provider's completion table
 * when a provider is installed, else the compiled-in backend's. Mirrors event_dispatch's
 * kl_event_* branch (loop->ops ? provider : builtin). */
static inline const KlCompletionOps *kl_comp_ops(const KlEventLoop *loop) {
    return loop->ops ? (const KlCompletionOps *)loop->ops->completion
                     : kl_comp_ops_builtin();
}

int kl_comp_drain(struct KlEventCtx *ctx, KlCompletionEvent *out, int max, int timeout_ms) {
    return kl_comp_ops(&ctx->loop)->drain(ctx, out, max, timeout_ms);
}

int kl_comp_prime_accepts(struct KlHttpServer *s) {
    return kl_comp_ops(&s->ev.loop)->prime_accepts(s);
}

int kl_comp_shutdown_accepts(struct KlHttpServer *s) {
    const KlCompletionOps *ops = kl_comp_ops(&s->ev.loop);
    /* ops is NULL on a readiness builtin (never reached — the caller gates on KL_EVENT_CAP_
     * COMPLETION); shutdown_accepts is NULL on an autonomous/no-accept backend. Both → success. */
    if (ops && ops->shutdown_accepts) return ops->shutdown_accepts(s);
    return 0;
}

/* Raw transport routers (KlStream form). The HTTP-adapter helpers kl_comp_post_recv/
 * _send/_sendfile (KlHttpConn form) live in completion_server.c and call these; the backend
 * behind the vtable does raw I/O only and never sees a KlHttpConn. */
int kl_comp_post_recv_raw(KlStream *stream, void *buf, size_t cap) {
    return kl_comp_ops(&stream->ctx->loop)->post_recv(stream, buf, cap);
}

int kl_comp_post_send_raw(KlStream *stream, const KlIoVec *iov, int iovcnt, size_t total) {
    return kl_comp_ops(&stream->ctx->loop)->post_send(stream, iov, iovcnt, total);
}

int kl_comp_post_accept(struct KlHttpServer *s) {
    return kl_comp_ops(&s->ev.loop)->post_accept(s);
}

int kl_comp_post_sendfile(KlHttpConn *c, const KlIoVec *head_iov, int head_n,
                          size_t head_total, int file_fd, uint64_t count) {
    return kl_comp_ops(&c->stream.ctx->loop)->post_sendfile(c, head_iov, head_n, head_total,
                                                            file_fd, count);
}

void kl_comp_cancel(struct KlEventCtx *ctx, KlSocketHandle fd) {
    kl_comp_ops(&ctx->loop)->cancel(ctx, fd);
}

int kl_comp_post_dgram_recv(struct KlEventCtx *ctx, const KlDgramRecvOp *op) {
    return kl_comp_ops(&ctx->loop)->post_dgram_recv(ctx, op);
}

int kl_comp_post_dgram_send(struct KlEventCtx *ctx, const KlDgramSendOp *op) {
    return kl_comp_ops(&ctx->loop)->post_dgram_send(ctx, op);
}

int kl_comp_cancel_dgram(struct KlEventCtx *ctx, struct KlDgramLife *life, KlDgramOpKind kind) {
    return kl_comp_ops(&ctx->loop)->cancel_dgram(ctx, life, kind);
}

KlDgramRetireResult kl_comp_retire_dgram(struct KlEventCtx *ctx, struct KlDgramLife *life,
                                         KlDgramOpKind kind, int *transport_err) {
    return kl_comp_ops(&ctx->loop)->retire_dgram(ctx, life, kind, transport_err);
}

int kl_comp_post_connect(struct KlEventCtx *ctx, KlSocketHandle fd,
                         const KlSockAddr *addr, void *watcher_udata) {
    return kl_comp_ops(&ctx->loop)->post_connect(ctx, fd, addr, watcher_udata);
}
