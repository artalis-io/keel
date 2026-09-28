/*
 * pipe_stream.c: a KlStream over a Windows Named Pipe (see <keel/pipe.h>).
 *
 * Generic substrate: it names no platform type and includes no platform header. It is the adapter
 * the KlStream contract expects: it installs the stream's completion-mode hooks (submit / arm /
 * cancel) and feeds completions back through kl_stream_on_recv / kl_stream_on_write_complete. The
 * Win32 mechanics sit behind two seams: platform_pipe.h (open / close the handle) and
 * completion_pipe.h (overlapped ReadFile / WriteFile on the IOCP port).
 *
 * LIFETIME, which is the part that matters. The whole KlPipeStream (the KlStream, the receive
 * buffer, the write queue, the pipe handle) is owned by one KlCompLife token, the transport-neutral
 * liveness + refcount token the datagram path uses. The owner holds one reference; every posted op
 * holds one more, transferred into its completion event and released after dispatch. The token's
 * final release (pipe_final) closes the handle and frees the memory, so:
 *
 *   - a ReadFile the kernel is still filling never lands in freed memory, even when the consumer
 *     calls kl_pipe_free with a read outstanding (logical close is not physical retirement, I3);
 *   - the handle is closed only after every op on it has retired, so no completion can target a
 *     recycled handle;
 *   - nothing here depends on how the op finished: success, EOF, cancellation, and loop teardown
 *     (which releases op refs without dispatching) all end at the same final release.
 *
 * The token's target is never marked dead: it stays valid for exactly as long as the memory does,
 * and `freed` alone decides whether the consumer still hears about anything.
 */
#include <keel/pipe.h>
#include <keel/stream_detail.h>   /* embed the KlStream */
#include <keel/event_ctx.h>       /* KlEventCtx.alloc */
#include <keel/allocator.h>
#include "completion.h"           /* KlCompletionEvent, KL_COMP_PIPE_* */
#include "completion_pipe.h"
#include "platform_pipe.h"
#include "completion_life.h"

#include <string.h>

struct KlPipeStream {
    KlStream      stream;
    struct KlEventCtx *ctx;
    KlAllocator  *alloc;
    KlCompLife  *life;
    KlPipeHandle *h;
    char         *rbuf;
    size_t        rcap;
    KlPipeDataFn  on_data;
    KlPipeCloseFn on_close;
    void         *user_data;
    int           freed;              /* kl_pipe_free ran: no further consumer callbacks */
    int           terminal_delivered; /* the one ok=0 delivery has happened */
};

/* ── Final release: every op has retired and the owner has let go ─────────────────────────── */

static void pipe_final(void *vp) {
    KlPipeStream *p = vp;
    KlAllocator *alloc = p->alloc;
    kl_plat_pipe_close(p->h);
    if (p->stream.wq_inited) {
        /* No op can reference the queue any more (this is the final release). A loop torn down with
         * a write still posted released that op's ref without dispatching it, so the stream never
         * saw the completion clear send_inflight; clear it so the queue is not refused and leaked. */
        p->stream.send_inflight = 0;
        (void)kl_stream_write_free(&p->stream);
    }
    if (p->rbuf) kl_free(alloc, p->rbuf, p->rcap);
    kl_free(alloc, p, sizeof(*p));
}

/* ── KlStream hooks ────────────────────────────────────────────────────────────────────────── */

static int pipe_post(KlPipeStream *p, KlPipeOpKind kind, const char *data, size_t len) {
    KlPipeIoOp op;
    memset(&op, 0, sizeof(op));
    op.h    = p->h;
    op.kind = kind;
    op.len  = len;
    op.life = p->life;
    if (kind == KL_PIPE_OP_READ) op.buf = p->rbuf;
    else                         op.data = data;
    kl_comp_life_retain(p->life);                 /* transferred into the op on success */
    if (kl_comp_pipe_post(p->ctx, &op) < 0) {
        kl_comp_life_release(p->life);            /* failed post took nothing */
        return -1;
    }
    return 0;
}

static int pipe_submit(void *vp, const char *data, size_t len) {
    return pipe_post(vp, KL_PIPE_OP_WRITE, data, len);
}

static int pipe_arm(void *vp) {
    KlPipeStream *p = vp;
    return pipe_post(p, KL_PIPE_OP_READ, NULL, p->rcap);
}

static int pipe_cancel_recv(void *vp) {
    KlPipeStream *p = vp;
    kl_comp_pipe_cancel(p->ctx, p->life, KL_PIPE_OP_READ);
    return 0;
}

static int pipe_cancel_send(void *vp) {
    KlPipeStream *p = vp;
    kl_comp_pipe_cancel(p->ctx, p->life, KL_PIPE_OP_WRITE);
    return 0;
}

static void pipe_deliver(void *vp, const char *buf, size_t len, int ok) {
    KlPipeStream *p = vp;
    if (!ok) {
        if (p->terminal_delivered) return;
        p->terminal_delivered = 1;
    }
    if (!p->freed) p->on_data(p->user_data, buf, len, ok);
}

static void pipe_on_close(void *vp) {
    KlPipeStream *p = vp;
    if (!p->freed && p->on_close) p->on_close(p->user_data);
}

/* ── Completion routing (the token's dispatch) ─────────────────────────────────────────────── */

static void pipe_dispatch(void *target, const KlCompletionEvent *ev) {
    KlPipeStream *p = target;   /* never NULL while the event holds a ref: the token is never killed */
    if (p) {
        if (ev->kind == KL_COMP_PIPE_READ) {
            /* -1 = the re-arm after this delivery failed (or the backend over-reported), which
             * closes the read side WITHOUT a terminal delivery; give the consumer its one terminal
             * so a read failure is never silent. */
            if (kl_stream_on_recv(&p->stream, ev->bytes, ev->ok) < 0)
                pipe_deliver(p, p->rbuf, 0, 0);
        } else if (ev->kind == KL_COMP_PIPE_WRITE) {
            (void)kl_stream_on_write_complete(&p->stream, ev->ok);   /* failure is sticky: the next
                                                                     * kl_stream_write reports it */
        }
    }
    if (!ev->retain_life) kl_comp_life_release(ev->life);   /* may be the final release */
}

/* ── Public surface ────────────────────────────────────────────────────────────────────────── */

static KlPipeStatus map_open(KlPipeOpenStatus s) {
    switch (s) {
    case KL_PIPE_OPEN_OK:          return KL_PIPE_OK;
    case KL_PIPE_OPEN_ABSENT:      return KL_PIPE_ABSENT;
    case KL_PIPE_OPEN_BUSY:        return KL_PIPE_BUSY;
    case KL_PIPE_OPEN_DENIED:      return KL_PIPE_DENIED;
    case KL_PIPE_OPEN_INVALID:     return KL_PIPE_INVALID;
    case KL_PIPE_OPEN_UNSUPPORTED: return KL_PIPE_UNSUPPORTED;
    case KL_PIPE_OPEN_ERROR:       default: return KL_PIPE_ERROR;
    }
}

KlPipeStatus kl_pipe_connect(struct KlEventCtx *ctx, const char *path, const KlPipeConfig *cfg,
                             KlPipeStream **out) {
    if (out) *out = NULL;
    if (!ctx || !path || !cfg || !cfg->on_data || !out || !ctx->alloc) return KL_PIPE_INVALID;
    /* Decided before any OS call: only the native IOCP engine carries pipe ops. */
    if (!kl_comp_pipe_available(ctx)) return KL_PIPE_UNSUPPORTED;

    KlAllocator *alloc = ctx->alloc;
    size_t rcap = cfg->read_capacity  ? cfg->read_capacity  : KL_PIPE_READ_CAP_DEFAULT;
    size_t wcap = cfg->write_capacity ? cfg->write_capacity : KL_PIPE_WRITE_CAP_DEFAULT;

    KlPipeStream *p = kl_malloc(alloc, sizeof(*p));
    if (!p) return KL_PIPE_NOMEM;
    memset(p, 0, sizeof(*p));
    p->ctx       = ctx;
    p->alloc     = alloc;
    p->rcap      = rcap;
    p->on_data   = cfg->on_data;
    p->on_close  = cfg->on_close;
    p->user_data = cfg->user_data;
    p->rbuf = kl_malloc(alloc, rcap);
    if (!p->rbuf) { kl_free(alloc, p, sizeof(*p)); return KL_PIPE_NOMEM; }
    if (kl_stream_init(&p->stream, p->rbuf, rcap) != 0) {
        kl_free(alloc, p->rbuf, rcap); kl_free(alloc, p, sizeof(*p));
        return KL_PIPE_ERROR;
    }
    p->life = kl_comp_life_create(alloc, p, pipe_final, p, pipe_dispatch);
    if (!p->life) { kl_free(alloc, p->rbuf, rcap); kl_free(alloc, p, sizeof(*p)); return KL_PIPE_NOMEM; }
    /* From here every failure is one owner release: pipe_final undoes whatever was built. */

    KlPipeStatus st = map_open(kl_plat_pipe_open_client(path, &p->h));
    if (st != KL_PIPE_OK) { p->h = NULL; kl_comp_life_release(p->life); return st; }
    if (kl_comp_pipe_attach(ctx, p->h) != 0) { kl_comp_life_release(p->life); return KL_PIPE_ERROR; }

    p->stream.alloc = alloc;
    p->stream.ctx   = ctx;
    if (kl_stream_write_init(&p->stream, alloc, wcap) != 0) {
        kl_comp_life_release(p->life);
        return KL_PIPE_NOMEM;
    }
    if (kl_stream_set_submit(&p->stream, pipe_submit, p, /*copying=*/1) != 0 ||
        kl_stream_read_init(&p->stream, /*completion=*/1, pipe_deliver, pipe_arm, NULL, p) != 0 ||
        kl_stream_close_init(&p->stream, pipe_on_close, p) != 0 ||
        kl_stream_set_cancel(&p->stream, pipe_cancel_recv, pipe_cancel_send) != 0) {
        kl_comp_life_release(p->life);
        return KL_PIPE_ERROR;
    }
    *out = p;
    return KL_PIPE_OK;
}

KlStream *kl_pipe_stream(KlPipeStream *p) {
    return p ? &p->stream : NULL;
}

void kl_pipe_free(KlPipeStream *p) {
    if (!p || p->freed) return;
    p->freed = 1;                       /* before the cancel: its on_close must not reach the consumer */
    (void)kl_stream_cancel(&p->stream); /* CancelIoEx whatever is outstanding; each still completes */
    kl_comp_life_release(p->life);     /* the owner ref; final once the last op has retired */
}

const char *kl_pipe_status_str(KlPipeStatus s) {
    switch (s) {
    case KL_PIPE_OK:          return "ok";
    case KL_PIPE_ABSENT:      return "absent";
    case KL_PIPE_BUSY:        return "busy";
    case KL_PIPE_DENIED:      return "denied";
    case KL_PIPE_INVALID:     return "invalid";
    case KL_PIPE_UNSUPPORTED: return "unsupported";
    case KL_PIPE_NOMEM:       return "nomem";
    case KL_PIPE_ERROR:       return "error";
    }
    return "unknown";
}
