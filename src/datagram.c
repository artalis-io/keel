/*
 * datagram.c — the public KlDatagram facade (Phase B, step 7B-3).
 *
 * A thin forwarding layer over the internal KlDgramCore assembly (7A). Its only real work is the TWO
 * adapter builders of the frozen §2.5 boundary: it manufactures the core's neutral hooks (submit / arm /
 * disarm / pull / cancel / retire / close_transport / deliver) by WRAPPING the existing backend seams —
 * NO per-backend code here. The mode is selected by the loop's capability (the same negotiation KlUdp
 * does), exactly two implementations:
 *
 *   completion (KL_EVENT_CAP_COMPLETION): submit → kl_comp_post_dgram_send; arm → kl_comp_post_dgram_recv;
 *       cancel → kl_comp_cancel_dgram; retire → kl_comp_retire_dgram; completions route back through the
 *       B.6 token's dispatch (kl_datagram_comp_dispatch, installed on the core's life at init).
 *   readiness (KL_EVENT_CAP_READINESS + sockets->dgram): submit → sockets->dgram->send (synchronous);
 *       arm/disarm → READ watcher on the fd; pull → sockets->dgram->recv; cancel → drop interest;
 *       retire → always RETIRED; close_transport → provider close (both modes).
 *
 * fd ownership transfers to KlDatagram ONLY on a successful kl_datagram_init (the core adopts it then);
 * the close machine's backend-retirement step closes it exactly once. KlUdp is untouched (D-COMPAT §6).
 *
 * 7B-7: the COMPLETION adapter additionally uses the GENERIC fd↔loop registration (kl_event_add at init,
 * kl_event_del at close) — the same lifecycle KlUdp uses for completion loops. This is NOT part of the
 * post/cancel/retire datagram seam; it is inert on io_uring/pollcomp and CreateIoCompletionPort on IOCP.
 * Register once before posting; on registration failure init fails without adopting the fd; at close the
 * coordinator retires every op, then kl_event_del, then the socket close — exactly once. (See the 7B-7
 * design resolution in docs/datagram_step7b_breakdown.md §2.5.)
 */

#include <keel/datagram.h>
#include <keel/datagram_detail.h>
#include <keel/event_ctx.h>     /* KlEventCtx, kl_watcher_add/_mod/_del, KlEventMask */
#include <keel/event.h>         /* KL_EVENT_READ / KL_EVENT_WRITE */

#include "datagram_core.h"
#include "completion.h"         /* KlCompletionEvent + kl_comp_* + KL_COMP_DGRAM_* */
#include "io_engine.h"          /* KlDgramSendOp / KlDgramRecvOp descriptors */
#include "socket.h"             /* KlSocketProvider, kl_sock_close, kl_sockdef_dgram, kl_sock_io_status */
#include "event_caps.h"         /* kl_event_caps */
#include "datagram_life.h"      /* kl_dgram_life_retain/_release */

#include <string.h>

/* ── provider accessors (mirror udp.c) ────────────────────────────────────────────────────────── */
static inline const KlDatagramOps *dg_ops(const KlDatagram *dg) {
    const KlSocketProvider *sp = dg->sockets;
    return sp ? sp->dgram : kl_sockdef_dgram();
}
static inline void *dg_sp_ctx(const KlDatagram *dg) {
    return dg->sockets ? dg->sockets->context : NULL;
}
static inline size_t dg_send_queued(const KlDatagram *dg)   { return kl_dgram_send_queued(&dg->core->send); }
static inline size_t dg_send_inflight(const KlDatagram *dg) { return kl_dgram_send_inflight(&dg->core->send); }

static void dg_on_ready(KlSocketHandle fd, KlEventMask ready, void *user_data);

/* ── readiness interest reconciliation ────────────────────────────────────────────────────────── */
/* Apply `want_mask` (READ from the recv machine's arm/disarm; WRITE while the send queue is non-empty)
 * to a single loop watcher. Completion mode never uses a watcher (posted ops carry their own interest). */
static void dg_reconcile(KlDatagram *dg) {
    if (dg->completion || !kl_handle_valid(dg->fd)) return;
    if (dg->want_mask == 0) {
        if (dg->read_armed) { kl_watcher_del(dg->ctx, dg->fd); dg->read_armed = 0; }
    } else if (!dg->read_armed) {
        if (kl_watcher_add(dg->ctx, dg->fd, dg->want_mask, dg_on_ready, dg) == 0) dg->read_armed = 1;
    } else {
        kl_watcher_mod(dg->ctx, dg->fd, dg->want_mask);
    }
}
/* Recompute the WRITE interest bit from the send-queue state, then reconcile (a would-block readiness
 * send needs a writable edge to flush; an empty queue drops it). */
static void dg_reconcile_write(KlDatagram *dg) {
    if (dg->completion) return;
    if (dg_send_queued(dg) > 0) dg->want_mask |= KL_EVENT_WRITE;
    else                        dg->want_mask &= ~(unsigned)KL_EVENT_WRITE;
    dg_reconcile(dg);
}

/* ── completion dispatch: the B.6 token routes this KlDatagram's completions here ──────────────── */
/* target = the live KlDgramCore (NULL once dead); ev->life is the transferred ref this handler releases
 * after dispatch. RECV fills the inbound slot's metadata from the event then retires+delivers; SEND
 * retires the single in-flight send. Never dereferences a dead/freed wrapper. */
void kl_datagram_comp_dispatch(void *target, const KlCompletionEvent *ev) {
    KlDgramLife *life = ev->life;
    KlDgramCore *core = (KlDgramCore *)target;
    switch (ev->kind) {
    case KL_COMP_DGRAM_RECV:
        if (!core) break;
        if (!ev->ok) {
            (void)kl_dgram_core_recv_on_complete(core, 0, 0);   /* failed/cancelled — retire, no delivery */
        } else {
            KlDgramSlot *in = kl_dgram_core_inbound_slot(core);
            if (in) {
                if (ev->buf && ev->buf != in->data && ev->bytes) {
                    size_t n = ev->bytes <= in->cap ? ev->bytes : in->cap;
                    memcpy(in->data, ev->buf, n);
                }
                in->flags = 0;
                if (kl_sockaddr_family(&ev->peer) != KL_AF_UNSPEC) in->peer = ev->peer;
                else memset(&in->peer, 0, sizeof(in->peer));
                if (kl_sockaddr_family(&ev->local) != KL_AF_UNSPEC) {
                    in->local = ev->local; in->flags |= KL_DGRAM_HAS_LOCAL;
                }
                if (ev->truncated) in->flags |= KL_DGRAM_TRUNCATED;
            }
            (void)kl_dgram_core_recv_on_complete(core, ev->bytes, 1);
        }
        break;
    case KL_COMP_DGRAM_SEND:
        if (core) (void)kl_dgram_core_send_on_complete(core, ev->ok);
        break;
    default: break;
    }
    /* 7B-9: release the event's ref UNLESS it is a BORROWED quarantine ref (retain_life=1) — then the
     * backend op keeps it forever (fail-closed; the recv machine still retired above via ok=0). Honoured
     * uniformly, including the dead-owner (core==NULL) break above. */
    if (!ev->retain_life)
        kl_dgram_life_release(life);
}

/* ── completion-mode adapters ─────────────────────────────────────────────────────────────────── */
static KlDgramSubmitResult dg_comp_submit(void *ctx, const void *data, size_t len,
                                          const KlSockAddr *peer, const KlSockAddr *local, int tos) {
    KlDatagram *dg = ctx;
    KlDgramSendOp op = { .fd = dg->fd, .data = data, .len = len, .dest = peer,
                         .src = local, .tos = tos, .life = kl_dgram_core_life(dg->core) };
    kl_dgram_life_retain(op.life);   /* transferred into the op on success */
    if (kl_comp_post_dgram_send(dg->ctx, &op) < 0) {
        kl_dgram_life_release(op.life);   /* failure → caller releases; backend took nothing */
        return KL_DGRAM_SUBMIT_ERROR;
    }
    return KL_DGRAM_SUBMIT_INFLIGHT;
}
static int dg_comp_arm(void *ctx) {
    KlDatagram *dg = ctx;
    KlDgramSlot *in = kl_dgram_core_inbound_slot(dg->core);
    KlDgramRecvOp op = { .fd = dg->fd, .buf = in ? in->data : NULL, .cap = in ? in->cap : 0,
                         .capture = 0, .life = kl_dgram_core_life(dg->core) };
    kl_dgram_life_retain(op.life);
    if (kl_comp_post_dgram_recv(dg->ctx, &op) < 0) {
        kl_dgram_life_release(op.life);
        return -1;
    }
    return 0;
}
static void dg_comp_cancel_send(void *ctx) {
    KlDatagram *dg = ctx;
    (void)kl_comp_cancel_dgram(dg->ctx, kl_dgram_core_life(dg->core), KL_DGRAM_OP_SEND);
}
static void dg_comp_cancel_recv(void *ctx) {
    KlDatagram *dg = ctx;
    (void)kl_comp_cancel_dgram(dg->ctx, kl_dgram_core_life(dg->core), KL_DGRAM_OP_RECV);
}
static KlDgramRetireResult dg_comp_retire(void *ctx, KlDgramOpKind kind, int *transport_err) {
    KlDatagram *dg = ctx;
    return kl_comp_retire_dgram(dg->ctx, kl_dgram_core_life(dg->core), kind, transport_err);
}

/* ── readiness-mode adapters ──────────────────────────────────────────────────────────────────── */
static KlDgramSubmitResult dg_rdy_submit(void *ctx, const void *data, size_t len,
                                         const KlSockAddr *peer, const KlSockAddr *local, int tos) {
    KlDatagram *dg = ctx;
    kl_ssize_t n = dg_ops(dg)->send(dg_sp_ctx(dg), dg->fd, data, len, peer, local, tos);
    if (n >= 0) return KL_DGRAM_SUBMIT_DONE;    /* UDP send is all-or-nothing */
    if (kl_sock_io_status(dg->sockets) == KL_IO_WOULD_BLOCK) return KL_DGRAM_SUBMIT_WOULDBLOCK;
    return KL_DGRAM_SUBMIT_ERROR;
}
static int dg_rdy_arm(void *ctx) {
    KlDatagram *dg = ctx;
    dg->want_mask |= KL_EVENT_READ;
    dg_reconcile(dg);
    return (dg->read_armed && (dg->want_mask & KL_EVENT_READ)) ? 0 : -1;   /* arm must report failure */
}
static void dg_rdy_disarm(void *ctx) {
    KlDatagram *dg = ctx;
    dg->want_mask &= ~(unsigned)KL_EVENT_READ;
    dg_reconcile(dg);
}
static int dg_rdy_pull(void *ctx, size_t *out_len) {
    KlDatagram *dg = ctx;
    KlDgramSlot *in = kl_dgram_core_inbound_slot(dg->core);
    KlSockAddr src;
    KlDgramRxMeta meta;
    kl_ssize_t n = dg_ops(dg)->recv(dg_sp_ctx(dg), dg->fd, in->data, in->cap, &src, &meta);
    if (n < 0) {
        if (kl_sock_io_status(dg->sockets) == KL_IO_WOULD_BLOCK) return 0;   /* drained */
        dg->last_error = KL_ERR_IO;
        return -1;
    }
    in->peer  = src;
    in->flags = 0;
    if (meta.has_local) { in->local = meta.local; in->flags |= KL_DGRAM_HAS_LOCAL; }
    if (meta.truncated) in->flags |= KL_DGRAM_TRUNCATED;
    *out_len = (size_t)n;
    return 1;
}
static void dg_rdy_cancel(void *ctx) { (void)ctx; }   /* readiness: interest is dropped at close_transport */
static KlDgramRetireResult dg_rdy_retire(void *ctx, KlDgramOpKind kind, int *transport_err) {
    (void)ctx; (void)kind;
    if (transport_err) *transport_err = 0;
    return KL_DGRAM_RETIRE_RETIRED;   /* synchronous disarm + close → retired immediately */
}

/* ── shared adapters ──────────────────────────────────────────────────────────────────────────── */
static void dg_close_transport(void *ctx, KlSocketHandle fd) {
    KlDatagram *dg = ctx;
    /* The close machine has already retired every op (cancel + drained) before this backend-retirement
     * step. Now, EXACTLY ONCE: remove the fd↔loop registration, then close the socket. Completion mode
     * uses the generic kl_event_del (7B-7, symmetric with the init kl_event_add — inert on
     * io_uring/pollcomp, the IOCP association drops on the close); readiness removes its watcher. */
    if (dg->completion) {
        if (dg->registered && kl_handle_valid(dg->fd)) { kl_event_del(&dg->ctx->loop, dg->fd); dg->registered = 0; }
    } else {
        if (dg->read_armed && kl_handle_valid(dg->fd)) { kl_watcher_del(dg->ctx, dg->fd); dg->read_armed = 0; }
        dg->want_mask = 0;
    }
    (void)kl_sock_close(dg->sockets, fd);
}
static void dg_deliver(void *ctx, const void *data, size_t len,
                       const KlSockAddr *peer, const KlSockAddr *local, unsigned flags) {
    KlDatagram *dg = ctx;
    if (flags & KL_DGRAM_TRUNCATED) dg->truncated++;
    if (dg->on_recv) dg->on_recv(dg->recv_ud, data, len, peer, local, flags);
}
static void dg_on_close(void *ctx, KlDatagramCloseResult result) {
    KlDatagram *dg = ctx;
    if (dg->on_close_cb) dg->on_close_cb(dg->close_ud, result);
}

/* the readiness recv/writable watcher */
static void dg_on_ready(KlSocketHandle fd, KlEventMask ready, void *user_data) {
    (void)fd;
    KlDatagram *dg = user_data;
    if ((ready & KL_EVENT_WRITE) && dg_send_queued(dg) > 0)
        (void)kl_dgram_core_send_flush(dg->core);
    if (ready & KL_EVENT_READ)
        (void)kl_dgram_core_recv_on_readable(dg->core);
    dg_reconcile_write(dg);
}

/* 7B-7 final PRE-ADOPTION hook: register the fd with the loop as the LAST fallible init step (after all
 * of KlDgramCore's allocations), immediately before adoption. Completion mode does the generic
 * kl_event_add (inert on io_uring/pollcomp, CreateIoCompletionPort on IOCP); readiness registers
 * per-arm via a watcher, so nothing here. On failure the core unwinds without adopting the fd — so on
 * IOCP the fd was never associated (CreateIoCompletionPort failed) and stays clean/re-usable. */
static int dg_prepare_register(void *ctx) {
    KlDatagram *dg = ctx;
    if (!dg->completion) return 0;
    if (kl_event_add(&dg->ctx->loop, dg->fd, KL_EVENT_READ, dg) < 0) {
        dg->last_error = KL_ERR_EVENT_ADD;
        return -1;
    }
    dg->registered = 1;
    return 0;
}

/* ══ public API ═══════════════════════════════════════════════════════════════════════════════ */

int kl_datagram_init(KlDatagram *dg, const KlDatagramConfig *cfg) {
    if (!dg || !cfg || !cfg->ctx || !cfg->alloc || !kl_handle_valid(cfg->fd)) {
        if (dg) { memset(dg, 0, sizeof(*dg)); dg->last_error = KL_ERR_INVALID_ARG; }
        return -1;
    }
    memset(dg, 0, sizeof(*dg));
    dg->ctx = cfg->ctx; dg->sockets = cfg->sockets; dg->alloc = cfg->alloc; dg->fd = cfg->fd;

    int completion = (kl_event_caps(&cfg->ctx->loop) & KL_EVENT_CAP_COMPLETION) ? 1 : 0;
    const KlDatagramOps *ops = cfg->sockets ? cfg->sockets->dgram : kl_sockdef_dgram();
    /* Neither a datagram-capable completion seam nor a readiness dgram provider → refuse pre-adoption. */
    if (!completion && (!ops || !ops->send || !ops->recv)) { dg->last_error = KL_ERR_INVALID_ARG; return -1; }
    dg->completion = completion;

    KlDgramCore *core = kl_malloc(cfg->alloc, sizeof(*core));
    if (!core) { dg->last_error = KL_ERR_ALLOC; return -1; }
    memset(core, 0, sizeof(*core));

    KlDgramCoreConfig cc;
    memset(&cc, 0, sizeof(cc));
    cc.alloc = cfg->alloc; cc.fd = cfg->fd; cc.completion = completion;
    cc.send_slots = cfg->send_slots; cc.send_slot_cap = cfg->send_slot_cap; cc.recv_cap = cfg->recv_cap;
    cc.caps = cfg->want_caps;
    cc.submit = completion ? dg_comp_submit : dg_rdy_submit; cc.submit_ctx = dg;
    cc.arm = completion ? dg_comp_arm : dg_rdy_arm;
    cc.disarm = completion ? NULL : dg_rdy_disarm;
    cc.pull = completion ? NULL : dg_rdy_pull;
    cc.recv_hook_ctx = dg;
    cc.deliver = dg_deliver; cc.deliver_ctx = dg;
    cc.cancel_send = completion ? dg_comp_cancel_send : dg_rdy_cancel;
    cc.cancel_recv = completion ? dg_comp_cancel_recv : dg_rdy_cancel;
    cc.cancel_ctx = dg;
    cc.retire = completion ? dg_comp_retire : dg_rdy_retire; cc.retire_ctx = dg;
    cc.close_transport = dg_close_transport; cc.transport_ctx = dg;
    cc.on_close = dg_on_close; cc.close_ctx = dg;
    cc.dispatch = completion ? kl_datagram_comp_dispatch : NULL;
    /* 7B-7: registration is the core's final pre-adoption step (dg_prepare_register), so it runs ONLY
     * after every allocation has succeeded and never leaves the fd associated on a failed init. */
    cc.on_prepared = dg_prepare_register; cc.prepared_ctx = dg;

    if (kl_dgram_core_init(core, &cc) != 0) {
        /* fd NOT adopted (caller keeps it). Registration was the last fallible step: it either did not
         * run (an earlier allocation failed) or failed itself — so the fd is NEVER left associated, and
         * dg->registered is 0. No kl_event_del is needed or correct here (IOCP cannot detach an ordinary
         * socket from its port; only closing it does). */
        kl_free(cfg->alloc, core, sizeof(*core));
        if (dg->last_error == KL_ERR_NONE) dg->last_error = KL_ERR_ALLOC;   /* the hook set EVENT_ADD on its own failure */
        return -1;
    }
    dg->core = core;   /* fd ownership has transferred to the core */
    return 0;
}

KlDatagramSendStatus kl_datagram_send(KlDatagram *dg, const KlDatagramMessage *m) {
    if (!dg || !dg->core) return KL_DATAGRAM_ERROR;
    KlDatagramSendStatus st = kl_dgram_core_send(dg->core, m);
    dg_reconcile_write(dg);   /* a queued (would-block) readiness send needs WRITE interest */
    return st;
}

int kl_datagram_recv_start(KlDatagram *dg, KlDatagramRecvFn on_recv, void *ud) {
    if (!dg || !dg->core) return -1;
    dg->on_recv = on_recv; dg->recv_ud = ud;
    return kl_dgram_core_recv_start(dg->core);   /* the machine drives arm → dg_rdy_arm sets interest */
}
void kl_datagram_pause(KlDatagram *dg)  { if (dg && dg->core) kl_dgram_core_pause(dg->core); }
int  kl_datagram_resume(KlDatagram *dg) { return (dg && dg->core) ? kl_dgram_core_resume(dg->core) : -1; }
void kl_datagram_recv_stop(KlDatagram *dg) { if (dg && dg->core) kl_dgram_core_recv_stop(dg->core); }

int kl_datagram_close_begin(KlDatagram *dg)  { return (dg && dg->core) ? kl_dgram_core_close_begin(dg->core)  : -1; }
int kl_datagram_close_cancel(KlDatagram *dg) { return (dg && dg->core) ? kl_dgram_core_close_cancel(dg->core) : -1; }

int kl_datagram_free(KlDatagram *dg) {
    if (!dg || !dg->core) return -1;
    if (kl_dgram_core_close_state(dg->core) != KL_DGRAM_CLOSE_CLOSED) { dg->last_error = KL_ERR_INVALID_ARG; return -1; }
    KlAllocator *a = dg->alloc; KlDgramCore *core = dg->core;
    if (kl_dgram_core_free(core) != 0) return -1;
    kl_free(a, core, sizeof(*core));
    memset(dg, 0, sizeof(*dg));   /* fully-detached — reusable (invariant 8) */
    return 0;
}

void kl_datagram_on_writable(KlDatagram *dg, KlDatagramWritableFn cb, void *ud) {
    if (dg && dg->core) kl_dgram_core_on_writable(dg->core, (KlDgramWritableFn)cb, ud);
}
void kl_datagram_on_drain(KlDatagram *dg, KlDatagramDrainFn cb, void *ud) {
    if (dg && dg->core) kl_dgram_core_on_drain(dg->core, (KlDgramDrainFn)cb, ud);
}
void kl_datagram_on_close(KlDatagram *dg, KlDatagramCloseFn cb, void *ud) {
    if (dg) { dg->on_close_cb = cb; dg->close_ud = ud; }
}

unsigned kl_datagram_caps(const KlDatagram *dg) { return (dg && dg->core) ? dg->core->caps : 0; }
KlDgramCloseState kl_datagram_close_state(const KlDatagram *dg) {
    return (dg && dg->core) ? kl_dgram_core_close_state(dg->core) : KL_DGRAM_CLOSE_CLOSED;
}
KlDatagramCloseResult kl_datagram_close_result(const KlDatagram *dg) {
    return (dg && dg->core) ? kl_dgram_core_close_result(dg->core) : KL_DGRAM_CLOSE_NONE;
}
size_t kl_datagram_send_queued(const KlDatagram *dg)   { return (dg && dg->core) ? dg_send_queued(dg)   : 0; }
size_t kl_datagram_send_inflight(const KlDatagram *dg) { return (dg && dg->core) ? dg_send_inflight(dg) : 0; }
uint64_t kl_datagram_dropped(const KlDatagram *dg)     { return dg ? dg->dropped   : 0; }
uint64_t kl_datagram_truncated(const KlDatagram *dg)   { return dg ? dg->truncated : 0; }
KlError  kl_datagram_last_error(const KlDatagram *dg)  { return dg ? dg->last_error : KL_ERR_INVALID_ARG; }
