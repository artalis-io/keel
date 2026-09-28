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
 *
 * LISTENER. A KlPipeListener is KlListener's object handoff family over pipe instances. Each posted
 * accept is a fresh server instance wrapped in its own KlPipeStream (the object a client connect
 * returns) with an overlapped ConnectNamedPipe outstanding on it (KL_PIPE_OP_ACCEPT, on that
 * stream's token). A connected instance is handed to the owner as that stream. A failed or cancelled
 * one is freed like any stream, so its handle closes only after the op has physically retired, and
 * instances are never recycled. The listener itself owns no handle.
 */
#include <keel/pipe.h>
#include <keel/stream_detail.h>   /* embed the KlStream */
#include <keel/listener_detail.h> /* embed the KlListener */
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
    KlCompLife   *life;
    KlPipeHandle *h;
    char         *rbuf;
    size_t        rcap;
    KlPipeDataFn  on_data;            /* NULL until bound (an accepted stream arrives unbound) */
    KlPipeCloseFn on_close;
    void         *user_data;
    int           freed;              /* kl_pipe_free ran: no further consumer callbacks */
    int           terminal_delivered; /* the one ok=0 delivery has happened */
    /* Listener instance only, while its ACCEPT op is outstanding: the listener waiting for it and
     * the links of that listener's pending set (for the batch cancel). NULL once it has retired. */
    struct KlPipeListener *pending;
    KlPipeStream  *pend_next, **pend_link;
};

struct KlPipeListener {
    KlListener         l;
    struct KlEventCtx *ctx;
    KlAllocator       *alloc;
    char              *path;          /* owned copy of the pipe name */
    size_t             path_size;
    KlPipeHandle      *first_h;       /* the name-claiming first instance, until the first arm */
    size_t             rcap, wcap;    /* for accepted streams */
    KlPipeAcceptFn     on_accept;
    KlPipeCloseFn      on_close;
    void              *user_data;
    KlPipeStream      *pending;       /* instances with a ConnectNamedPipe outstanding */
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
    if (kind == KL_PIPE_OP_READ)       op.buf = p->rbuf;
    else if (kind == KL_PIPE_OP_WRITE) op.data = data;
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
    if (!p->freed && p->on_data) p->on_data(p->user_data, buf, len, ok);
}

static void pipe_on_close(void *vp) {
    KlPipeStream *p = vp;
    if (!p->freed && p->on_close) p->on_close(p->user_data);
}

/* ── Completion routing (the token's dispatch) ─────────────────────────────────────────────── */

static void listener_accept_done(KlPipeStream *p, int ok);

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
        } else if (ev->kind == KL_COMP_PIPE_ACCEPT) {
            listener_accept_done(p, ev->ok);
        }
    }
    if (!ev->retain_life) kl_comp_life_release(ev->life);   /* may be the final release */
}

/* ── Construction shared by the client and the listener ────────────────────────────────────── */

static KlPipeStatus map_open(KlPipeOpenStatus s) {
    switch (s) {
    case KL_PIPE_OPEN_OK:          return KL_PIPE_OK;
    case KL_PIPE_OPEN_ABSENT:      return KL_PIPE_ABSENT;
    case KL_PIPE_OPEN_BUSY:        return KL_PIPE_BUSY;
    case KL_PIPE_OPEN_DENIED:      return KL_PIPE_DENIED;
    case KL_PIPE_OPEN_INVALID:     return KL_PIPE_INVALID;
    case KL_PIPE_OPEN_UNSUPPORTED: return KL_PIPE_UNSUPPORTED;
    case KL_PIPE_OPEN_IN_USE:      return KL_PIPE_IN_USE;
    case KL_PIPE_OPEN_ERROR:       default: return KL_PIPE_ERROR;
    }
}

/* Build a stream around the already-open handle `h` and attach it to the port. Takes ownership of
 * `h` in every case: on failure it is closed (directly, or by the final release). */
static KlPipeStatus pipe_new(struct KlEventCtx *ctx, KlPipeHandle *h, size_t rcap, size_t wcap,
                             KlPipeStream **out) {
    KlAllocator *alloc = ctx->alloc;
    if (!rcap) rcap = KL_PIPE_READ_CAP_DEFAULT;
    if (!wcap) wcap = KL_PIPE_WRITE_CAP_DEFAULT;

    KlPipeStream *p = kl_malloc(alloc, sizeof(*p));
    if (!p) { kl_plat_pipe_close(h); return KL_PIPE_NOMEM; }
    memset(p, 0, sizeof(*p));
    p->ctx   = ctx;
    p->alloc = alloc;
    p->rcap  = rcap;
    p->rbuf  = kl_malloc(alloc, rcap);
    if (!p->rbuf) { kl_free(alloc, p, sizeof(*p)); kl_plat_pipe_close(h); return KL_PIPE_NOMEM; }
    if (kl_stream_init(&p->stream, p->rbuf, rcap) != 0) {
        kl_free(alloc, p->rbuf, rcap); kl_free(alloc, p, sizeof(*p)); kl_plat_pipe_close(h);
        return KL_PIPE_ERROR;
    }
    p->life = kl_comp_life_create(alloc, p, pipe_final, p, pipe_dispatch);
    if (!p->life) {
        kl_free(alloc, p->rbuf, rcap); kl_free(alloc, p, sizeof(*p)); kl_plat_pipe_close(h);
        return KL_PIPE_NOMEM;
    }
    /* From here every failure is one owner release: pipe_final undoes whatever was built. */
    p->h = h;
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

/* ── Client ────────────────────────────────────────────────────────────────────────────────── */

KlPipeStatus kl_pipe_connect(struct KlEventCtx *ctx, const char *path, const KlPipeConfig *cfg,
                             KlPipeStream **out) {
    if (out) *out = NULL;
    if (!ctx || !path || !cfg || !cfg->on_data || !out || !ctx->alloc) return KL_PIPE_INVALID;
    /* Decided before any OS call: only the native IOCP engine carries pipe ops. */
    if (!kl_comp_pipe_available(ctx)) return KL_PIPE_UNSUPPORTED;

    KlPipeHandle *h = NULL;
    KlPipeStatus st = map_open(kl_plat_pipe_open_client(path, &h));
    if (st != KL_PIPE_OK) return st;
    KlPipeStream *p = NULL;
    st = pipe_new(ctx, h, cfg->read_capacity, cfg->write_capacity, &p);
    if (st != KL_PIPE_OK) return st;
    p->on_data   = cfg->on_data;
    p->on_close  = cfg->on_close;
    p->user_data = cfg->user_data;
    *out = p;
    return KL_PIPE_OK;
}

KlStream *kl_pipe_stream(KlPipeStream *p) {
    return p ? &p->stream : NULL;
}

int kl_pipe_bind(KlPipeStream *p, KlPipeDataFn on_data, KlPipeCloseFn on_close, void *user_data) {
    if (!p || !on_data) return -1;
    p->on_data   = on_data;
    p->on_close  = on_close;
    p->user_data = user_data;
    return 0;
}

void kl_pipe_free(KlPipeStream *p) {
    if (!p || p->freed) return;
    p->freed = 1;                       /* before the cancel: its on_close must not reach the consumer */
    (void)kl_stream_cancel(&p->stream); /* CancelIoEx whatever is outstanding; each still completes */
    kl_comp_life_release(p->life);      /* the owner ref; final once the last op has retired */
}

/* ── Listener: KlListener's object handoff family over pipe instances ──────────────────────── */

static void pend_link(KlPipeListener *pl, KlPipeStream *p) {
    p->pending   = pl;
    p->pend_next = pl->pending;
    p->pend_link = &pl->pending;
    if (pl->pending) pl->pending->pend_link = &p->pend_next;
    pl->pending = p;
}

static void pend_unlink(KlPipeStream *p) {
    if (!p->pend_link) return;
    *p->pend_link = p->pend_next;
    if (p->pend_next) p->pend_next->pend_link = p->pend_link;
    p->pend_next = NULL;
    p->pend_link = NULL;
    p->pending   = NULL;
}

/* Post one accept: a server instance (the name-claiming first one, else a fresh one) wrapped in a
 * stream, with its ConnectNamedPipe outstanding. Never completes inline: the backend self-queues even
 * an already-connected client, so the KlListener pump always sees an async post. */
static int listener_arm(void *vp) {
    KlPipeListener *pl = vp;
    KlPipeHandle *h = pl->first_h;
    pl->first_h = NULL;
    if (!h && kl_plat_pipe_create_instance(pl->path, /*first=*/0, &h) != KL_PIPE_OPEN_OK) return -1;
    KlPipeStream *p = NULL;
    if (pipe_new(pl->ctx, h, pl->rcap, pl->wcap, &p) != KL_PIPE_OK) return -1;   /* h consumed */
    pend_link(pl, p);
    if (pipe_post(p, KL_PIPE_OP_ACCEPT, NULL, 0) != 0) {
        pend_unlink(p);
        kl_pipe_free(p);
        return -1;
    }
    return 0;
}

/* The ONE batch cancel KlListener asks for on close: cancel every outstanding ConnectNamedPipe. Each
 * still completes (aborted, or connected if it won the race) and retires through the dispatch. */
static void listener_cancel(void *vp) {
    KlPipeListener *pl = vp;
    for (KlPipeStream *p = pl->pending; p; p = p->pend_next)
        kl_comp_pipe_cancel(pl->ctx, p->life, KL_PIPE_OP_ACCEPT);
}

static void listener_on_accept_obj(void *vp, void *conn, KlSlotLease lease) {
    KlPipeListener *pl = vp;
    kl_slot_lease_release(&lease);          /* no pool behind a pipe listener: a no-op lease */
    pl->on_accept(pl->user_data, (KlPipeStream *)conn);
}

static void listener_dispose_obj(void *vp, void *conn) {
    (void)vp;
    kl_pipe_free((KlPipeStream *)conn);     /* its handle closes once nothing references it */
}

static void listener_on_close(void *vp) {
    KlPipeListener *pl = vp;
    if (pl->on_close) pl->on_close(pl->user_data);   /* may free pl: touch nothing after */
}

/* A pending instance's ConnectNamedPipe completed. Hand the connected stream to KlListener (which
 * passes it to the owner, or disposes it while closing), or free the instance and retire the accept
 * as failed. */
static void listener_accept_done(KlPipeStream *p, int ok) {
    KlPipeListener *pl = p->pending;
    pend_unlink(p);
    if (!pl) return;                        /* not a listener instance: nothing to do */
    if (ok) {
        kl_listener_on_accepted_obj(&pl->l, p);
    } else {
        kl_pipe_free(p);                    /* closes the instance once this event releases */
        kl_listener_on_accept_failed(&pl->l, -1);   /* may detach → on_close → free pl */
    }
}

KlPipeStatus kl_pipe_listen(struct KlEventCtx *ctx, const char *path, const KlPipeListenConfig *cfg,
                            KlPipeListener **out) {
    if (out) *out = NULL;
    if (!ctx || !path || !cfg || !cfg->on_accept || !out || !ctx->alloc || cfg->instances < 0)
        return KL_PIPE_INVALID;
    if (!kl_comp_pipe_available(ctx)) return KL_PIPE_UNSUPPORTED;

    KlAllocator *alloc = ctx->alloc;
    KlPipeListener *pl = kl_malloc(alloc, sizeof(*pl));
    if (!pl) return KL_PIPE_NOMEM;
    memset(pl, 0, sizeof(*pl));
    pl->ctx       = ctx;
    pl->alloc     = alloc;
    pl->rcap      = cfg->read_capacity;
    pl->wcap      = cfg->write_capacity;
    pl->on_accept = cfg->on_accept;
    pl->on_close  = cfg->on_close;
    pl->user_data = cfg->user_data;
    pl->path_size = strlen(path) + 1;
    pl->path = kl_malloc(alloc, pl->path_size);
    if (!pl->path) { kl_free(alloc, pl, sizeof(*pl)); return KL_PIPE_NOMEM; }
    memcpy(pl->path, path, pl->path_size);

    /* Claim the name NOW, so a squatter or a second server is reported here rather than later. */
    KlPipeStatus st = map_open(kl_plat_pipe_create_instance(pl->path, /*first=*/1, &pl->first_h));
    if (st != KL_PIPE_OK) {
        kl_free(alloc, pl->path, pl->path_size); kl_free(alloc, pl, sizeof(*pl));
        return st;
    }

    KlListenerHooks hooks;
    memset(&hooks, 0, sizeof(hooks));
    hooks.arm_accept    = listener_arm;
    hooks.cancel_accept = listener_cancel;
    hooks.on_accept_obj = listener_on_accept_obj;
    hooks.dispose_obj   = listener_dispose_obj;
    hooks.on_close      = listener_on_close;
    int window = cfg->instances ? cfg->instances : KL_PIPE_LISTEN_INSTANCES_DEFAULT;
    if (kl_listener_init(&pl->l, /*completion_mode=*/1, &hooks, pl) != 0 ||
        kl_listener_set_accept_window(&pl->l, window) != 0) {
        kl_plat_pipe_close(pl->first_h);
        kl_free(alloc, pl->path, pl->path_size); kl_free(alloc, pl, sizeof(*pl));
        return KL_PIPE_INVALID;
    }
    /* Start: posts up to `window` instances. A hard arm failure closes the listener, and with nothing
     * posted it detaches at once; report that as a failed listen rather than hand back a dead one. */
    (void)kl_listener_start(&pl->l);
    if (kl_listener_state(&pl->l) != KL_LISTENER_STATE_LISTENING) {
        if (kl_listener_is_detached(&pl->l)) {
            kl_plat_pipe_close(pl->first_h);   /* NULL unless the first arm never ran */
            kl_free(alloc, pl->path, pl->path_size); kl_free(alloc, pl, sizeof(*pl));
            return KL_PIPE_ERROR;
        }
        /* closing with some instances posted: they retire through the loop; hand it back so the
         * owner can observe on_close and free it */
    }
    *out = pl;
    return KL_PIPE_OK;
}

int kl_pipe_listener_close(KlPipeListener *pl) {
    return pl ? kl_listener_close(&pl->l) : -1;
}

int kl_pipe_listener_free(KlPipeListener *pl) {
    if (!pl) return -1;
    if (!kl_listener_is_detached(&pl->l)) return -1;   /* posted accepts still reference it */
    KlAllocator *alloc = pl->alloc;
    kl_plat_pipe_close(pl->first_h);                   /* NULL unless the first arm never ran */
    kl_free(alloc, pl->path, pl->path_size);
    kl_free(alloc, pl, sizeof(*pl));
    return 0;
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
    case KL_PIPE_IN_USE:      return "in_use";
    }
    return "unknown";
}
