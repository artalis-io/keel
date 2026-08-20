/*
 * datagram.c — the public KlDatagram facade (Phase B, step 7B-3).
 *
 * A thin forwarding layer over the internal KlDgramCore assembly (7A). Its only real work is the TWO
 * adapter builders of the frozen §2.5 boundary: it manufactures the core's neutral hooks (submit / arm /
 * disarm / pull / cancel / retire / close_transport / deliver) by WRAPPING the existing backend seams —
 * NO per-backend code here. The mode is selected by the loop's capability (XXX * uses everywhere), exactly two implementations:
 *
 *   completion (KL_EVENT_CAP_COMPLETION): submit → kl_comp_post_dgram_send; arm → kl_comp_post_dgram_recv;
 *       cancel → kl_comp_cancel_dgram; retire → kl_comp_retire_dgram; completions route back through the
 *       B.6 token's dispatch (kl_datagram_comp_dispatch, installed on the core's life at init).
 *   readiness (KL_EVENT_CAP_READINESS + sockets->dgram): submit → sockets->dgram->send (synchronous);
 *       arm/disarm → READ watcher on the fd; pull → sockets->dgram->recv; cancel → drop interest;
 *       retire → always RETIRED; close_transport → provider close (both modes).
 *
 * fd ownership transfers to KlDatagram ONLY on a successful kl_datagram_init (the core adopts it then);
 * the close machine's backend-retirement step closes it exactly once.
 *
 * 7B-7: the COMPLETION adapter additionally uses the GENERIC fd↔loop registration (kl_event_add at init,
 * kl_event_del at close) — the generic completion-loop lifecycle. This is NOT part of the
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
#include "datagram_batch.h"     /* KlDatagramBatch layout — M5.2a send_batch drives the core send queue */
#include "completion.h"         /* KlCompletionEvent + kl_comp_* + KL_COMP_DGRAM_* */
#include "io_engine.h"          /* KlDgramSendOp / KlDgramRecvOp descriptors */
#include "socket.h"             /* KlSocketProvider, kl_sock_close, kl_sockdef_dgram, kl_sock_io_status */
#include "event_caps.h"         /* kl_event_caps */
#include "datagram_life.h"      /* kl_dgram_life_retain/_release */
#include "datagram_open.h"      /* KlDatagramReclaimFn + kl_datagram_teardown */

#include <string.h>

/* ── provider accessors ────────────────────────────────────────────────────────── */
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
static void dg_on_send_drop(void *ctx);   /* M5.2a: recoverable batch-drop reporter (wired at init) */
static KlDgramSubmitResult dg_submit_gso(void *ctx, void *owner, const void *buf, size_t total,
                                         size_t seg, const KlSockAddr *peer, int tos);  /* M5.2b */
static void dg_on_gso_done(void *ctx, void *owner);   /* M5.2b: clears the batch's gso_busy */

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
                /* M6.0a: stash the received TOS on the inbound slot (read by kl_datagram_recv_tos during
                 * on_recv). Gate on the socket's accepted RX_TOS mask: a backend that does not capture TOS
                 * leaves ev->tos == 0 (a VALID byte), so only trust it when TOS capture was enabled. */
                in->tos = (kl_dgram_core_accepted_rx_caps(core) & KL_DGRAM_RX_TOS) ? ev->tos : -1;
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
    /* Capture the local (dest) address on the completion recv — consistent with the readiness recv,
     * which always parses the pktinfo cmsg — so a wildcard-bound source-pinned reply can learn its
     * local addr. Harmless when the socket has no IP_PKTINFO (no cmsg → no local). M6.0a: ALSO request
     * the received-TOS cmsg when the socket has RX_TOS capture enabled (accepted_rx_caps), so
     * kl_datagram_recv_tos can surface it — the capture mask only tells the backend which cmsgs to parse. */
    unsigned capture = KL_DGRAM_RX_PKTINFO;
    if (kl_dgram_core_accepted_rx_caps(dg->core) & KL_DGRAM_RX_TOS) capture |= KL_DGRAM_RX_TOS;
    KlDgramRecvOp op = { .fd = dg->fd, .buf = in ? in->data : NULL, .cap = in ? in->cap : 0,
                         .capture = capture, .life = kl_dgram_core_life(dg->core) };
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
    /* M6.0a: received TOS (read by kl_datagram_recv_tos in on_recv). GATE on the socket's accepted RX_TOS
     * mask (as the completion dispatch does) so behavior is backend-INDEPENDENT — a provider that surfaces
     * meta.tos on a socket that never enabled TOS capture must NOT leak it. */
    in->tos = (kl_dgram_core_accepted_rx_caps(dg->core) & KL_DGRAM_RX_TOS) ? meta.tos : -1;
    *out_len = (size_t)n;
    return 1;
}
/* M5.3 — the borrowed-view yield adapter (readiness batch mode). Yields one logical datagram from the
 * attached RECV batch cursor, refilling via one recv_batch (or a single recv when RX_BATCH is absent).
 * A GRO-coalesced slot is split per-segment by default; with on_recv_segments the whole coalesced
 * buffer is yielded (recv_seg_size scratch carries the segment size for the deliver adapter). */
static int dg_rdy_pull_batch(void *ctx, KlDgramRxView *v, int allow_refill) {
    KlDatagram *dg = ctx;
    KlDatagramBatch *b = (KlDatagramBatch *)dg->core->ext;
    dg->recv_seg_size = 0;   /* default: a plain/split datagram (deliver → on_recv) */

    if (b->cursor_i >= b->n_filled) {   /* cursor drained */
        if (!allow_refill) return 0;    /* §5.3 held-drain: never read new kernel data */
        if (b->rx_block) {              /* one recvmmsg into rx_slots[0..n_filled) */
            int r = dg_ops(dg)->recv_batch(dg_sp_ctx(dg), dg->fd, b->rx_block, b->rx_slots, b->n_slots);
            if (r < 0) { if (kl_sock_io_status(dg->sockets) == KL_IO_WOULD_BLOCK) return 0;
                         dg->last_error = KL_ERR_IO; return -1; }
            if (r > b->n_slots) { dg->last_error = KL_ERR_IO; return -1; }   /* provider over-report → fail */
            b->n_filled = r;
        } else {                        /* single-recv fallback into the fallback buffer */
            KlSockAddr src; KlDgramRxMeta meta;
            kl_ssize_t n = dg_ops(dg)->recv(dg_sp_ctx(dg), dg->fd, b->rx_fallback, b->slot_bufsz, &src, &meta);
            if (n < 0) { if (kl_sock_io_status(dg->sockets) == KL_IO_WOULD_BLOCK) return 0;
                         dg->last_error = KL_ERR_IO; return -1; }
            /* The provider reports the DATAGRAM length (may exceed the buffer). Clamp the captured length
             * to slot_bufsz and mark TRUNCATED if the datagram was larger. */
            size_t captured = (size_t)n; int trunc = meta.truncated;
            if (captured > b->slot_bufsz) { captured = b->slot_bufsz; trunc = 1; }
            b->rx_slots[0].data = b->rx_fallback; b->rx_slots[0].len = captured;
            b->rx_slots[0].src = src; b->rx_slots[0].meta = meta; b->rx_slots[0].meta.truncated = trunc;
            b->n_filled = 1;
        }
        if (b->n_filled == 0) return 0;   /* drained (EAGAIN) */
        b->cursor_i = 0; b->seg_off = 0;
    }

    KlDgramRxSlot *slot = &b->rx_slots[b->cursor_i];
    /* Defensive bound: a provider over-reporting length past the slot buffer delivers a captured prefix
     * with TRUNCATED — set the flag when EITHER the provider marked it OR the length was clamped. */
    int    over  = slot->len > b->slot_bufsz;
    size_t slen  = over ? b->slot_bufsz : slot->len;
    int    trunc = slot->meta.truncated || over;

    /* LATCH the GRO delivery mode when entering a slot (seg_off == 0) and keep it until the slot advances
     * — a callback that (un)registers on_recv_segments mid-split cannot change the mode for the remaining
     * segments. GRO split is active only under the §6.2 two-part gate (b->gro_active); otherwise the
     * provider's meta.gro_seg is IGNORED. */
    if (b->seg_off == 0) {
        b->slot_gro   = b->gro_active ? slot->meta.gro_seg : 0;
        b->slot_split = (b->slot_gro > 0) && (dg->on_recv_segments == NULL);
    }
    size_t avail = (slen > b->seg_off) ? slen - b->seg_off : 0;

    /* Borrowed view at seg_off. Avoid NULL+off arithmetic (a zero-length or NULL provider buffer):
     * reference the base directly when seg_off == 0. */
    v->data = (b->seg_off == 0) ? slot->data : (const void *)((const unsigned char *)slot->data + b->seg_off);
    v->peer = slot->src;
    v->flags = 0;
    if (slot->meta.has_local) { v->local = slot->meta.local; v->flags |= KL_DGRAM_HAS_LOCAL; }
    if (trunc)                  v->flags |= KL_DGRAM_TRUNCATED;
    /* M6.0a: keep kl_datagram_recv_tos correct in batch mode too — the accessor reads the inbound slot,
     * which the batch path does not otherwise fill, so mirror this datagram's TOS onto it. GATE on the
     * accepted RX_TOS mask (same as the single-recv + completion paths) so it is backend-independent. */
    { KlDgramSlot *in = kl_dgram_core_inbound_slot(dg->core);
      if (in) in->tos = (kl_dgram_core_accepted_rx_caps(dg->core) & KL_DGRAM_RX_TOS) ? slot->meta.tos : -1; }

    if (b->slot_split && avail > (size_t)b->slot_gro) {   /* split: yield one segment, stay on this slot */
        v->len = (size_t)b->slot_gro;
        b->seg_off += (size_t)b->slot_gro;
    } else {                              /* whole remaining slot (plain / last segment / coalesced) */
        v->len = avail;
        if (!b->slot_split && b->slot_gro > 0 && dg->on_recv_segments)
            dg->recv_seg_size = (size_t)b->slot_gro;   /* whole-coalesced buffer → on_recv_segments */
        b->cursor_i++; b->seg_off = 0;
    }
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
    /* M5.3: a whole GRO-coalesced buffer (recv_seg_size scratch set by the yield adapter) routes to
     * on_recv_segments; everything else (plain datagram or a per-segment split) to on_recv. */
    if (dg->recv_seg_size && dg->on_recv_segments)
        dg->on_recv_segments(dg->recv_seg_ud, data, len, dg->recv_seg_size, peer, local, flags);
    else if (dg->on_recv)
        dg->on_recv(dg->recv_ud, data, len, peer, local, flags);
}
static void dg_on_close(void *ctx, KlDatagramCloseResult result) {
    KlDatagram *dg = ctx;
    if (dg->on_close_cb) dg->on_close_cb(dg->close_ud, result);
}

/* the readiness recv/writable watcher */
static void dg_on_ready(KlSocketHandle fd, KlEventMask ready, void *user_data) {
    (void)fd;
    KlDatagram *dg = user_data;
    /* Hold a frame for the WHOLE dispatch: send_flush/recv_on_readable deliver callbacks (on_drain /
     * on_writable / on_recv) from which the consumer may destroy the owner (kl_datagram_teardown). The
     * bracket keeps busy > 0 so that teardown's terminal DEFERS to dispatch_end below — the last action —
     * instead of firing inside the inner op leave and freeing `dg`/`core` while we still touch them
     * (dg_reconcile_write). `core` is captured up front; the body cannot free it (deferred), and
     * dispatch_end may. */
    KlDgramCore *core = dg->core;
    kl_dgram_core_dispatch_begin(core);
    if ((ready & KL_EVENT_WRITE) && dg_send_queued(dg) > 0)
        (void)kl_dgram_core_send_flush(core);
    if (ready & KL_EVENT_READ)
        (void)kl_dgram_core_recv_on_readable(core);
    dg_reconcile_write(dg);
    kl_dgram_core_dispatch_end(core);   /* LAST touch of `dg`/`core` — may run the deferred terminal + free */
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
    return kl_datagram_init_ex(dg, cfg, 0);   /* 0 = SLOT policy (byte gate off) — the STABLE default */
}

int kl_datagram_init_ex(KlDatagram *dg, const KlDatagramConfig *cfg, size_t send_byte_budget) {
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

    /* M2: derive the provider's capabilities for THIS fd, then fail-loud gate want_caps. A NULL caps op
     * reports NO optional caps (a function signature never proves behavior). A want_caps requesting a cap
     * the provider does not support fails init HERE — before fd adoption — so the caller retains the fd. */
    {
        void *sp_ctx = cfg->sockets ? cfg->sockets->context : NULL;
        unsigned provider_caps = (ops && ops->caps) ? ops->caps(sp_ctx, cfg->fd) : 0;
        if (cfg->want_caps & ~provider_caps) { dg->last_error = KL_ERR_UNSUPPORTED; return -1; }
        dg->provider_caps = provider_caps;
    }
    /* M6.0a: the granted cap set = the REQUIRED caps (already gated above) PLUS the OPTIONAL caps the
     * provider actually supports on this fd. optional_caps a provider lacks are silently dropped (never
     * an init failure), so a wrapper can request every capability its API may use without regressing a
     * reduced provider that only does unconnected send_to. This is what the send machine reads (core->caps)
     * to admit connected/source-pin/TOS sends, and what kl_datagram_caps reports. */
    unsigned granted_caps = cfg->want_caps | (cfg->optional_caps & dg->provider_caps);

    KlDgramCore *core = kl_malloc(cfg->alloc, sizeof(*core));
    if (!core) { dg->last_error = KL_ERR_ALLOC; return -1; }
    memset(core, 0, sizeof(*core));

    KlDgramCoreConfig cc;
    memset(&cc, 0, sizeof(cc));
    cc.alloc = cfg->alloc; cc.fd = cfg->fd; cc.completion = completion;
    cc.send_slots = cfg->send_slots; cc.send_slot_cap = cfg->send_slot_cap; cc.recv_cap = cfg->recv_cap;
    cc.send_byte_budget = send_byte_budget;   /* M1: 0 = SLOT, >0 = BOTH */
    cc.caps = granted_caps;   /* M6.0a: want_caps + opportunistically-granted optional_caps */
    /* M5.1: store the enabled-per-socket RX mask, masked to KNOWN KL_DGRAM_RX_* bits (O-E) — separate
     * from provider support; drives the GRO-activation predicate (provider GRO support AND this). */
    cc.accepted_rx_caps = cfg->accepted_rx_caps &
        (KL_DGRAM_RX_PKTINFO | KL_DGRAM_RX_GRO | KL_DGRAM_RX_TOS);
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
    kl_dgram_core_on_drop(core, dg_on_send_drop, dg);   /* M5.2a: surface recoverable batch drops */
    kl_dgram_core_set_gso_cbs(core, dg_submit_gso, dg, dg_on_gso_done, dg);   /* M5.2b: GSO submit + done */
    return 0;
}

/* ── D1: public socket convenience (create + configure + bind + adopt) ────────────────────────────
 * Reuses the accepted M0 preparation (kl_datagram_open) internally, passing the socket config straight
 * through (D3 collapsed the config to a single type — the provider seam takes KlDatagramSocketConfig). */
int kl_datagram_socket_init(KlDatagram *dg, const KlDatagramSocketConfig *cfg) {
    if (!dg) return -1;
    memset(dg, 0, sizeof(*dg));
    if (!cfg || !cfg->ctx) { dg->last_error = KL_ERR_INVALID_ARG; return -1; }
    /* Validate queue_policy BEFORE creating any fd (P2). */
    if (cfg->queue_policy != KL_DATAGRAM_QUEUE_DEFAULT &&
        cfg->queue_policy != KL_DATAGRAM_QUEUE_SLOT &&
        cfg->queue_policy != KL_DATAGRAM_QUEUE_BOTH) {
        dg->last_error = KL_ERR_INVALID_ARG; return -1;
    }
    int completion = (kl_event_caps(&cfg->ctx->loop) & KL_EVENT_CAP_COMPLETION) ? 1 : 0;
    KlAllocator *alloc = cfg->alloc ? cfg->alloc : cfg->ctx->alloc;
    /* P1: resolve ONE effective provider. "NULL = ctx default" means the EVENT CONTEXT's provider
     * (cfg->ctx->sockets), not the built-in POSIX default — else a custom lwIP/EFI event context would be
     * mixed with POSIX datagram ops. Use it consistently for open, adoption, and every pre-adoption close. */
    const KlSocketProvider *sockets = cfg->sockets ? cfg->sockets : cfg->ctx->sockets;

    /* M0 prep takes the socket config directly (D3: single config type). Only two adjustments vs the
     * caller's config: GRO is readiness-only (M5.4) — never enable UDP_GRO capture on a completion loop
     * (the completion recv cannot split a coalesced datagram); and multicast_group is joined post-init
     * via M2, never at configure(). (Batch sizing is a kl_datagram_batch_create arg, not a socket option.) */
    size_t recv_cap = cfg->recv_cap ? cfg->recv_cap : 2048;
    if (recv_cap > 65535) recv_cap = 65535;
    KlDatagramSocketConfig oc = *cfg;
    oc.recv_gro = (!completion && cfg->recv_gro) ? 1 : 0;
    oc.multicast_group = NULL;   /* joined post-init (M2) */
    oc.alloc = alloc;

    KlDatagramPrep prep;
    if (kl_datagram_open(sockets, &oc, &prep) != 0) {
        dg->last_error = prep.err;   /* open closed its own fd on failure — nothing to reclaim */
        return -1;
    }
    /* Required RX-capture gate (P1-D): fail-loud, close the prepared fd exactly once. */
    if (cfg->want_rx_caps & ~prep.rx_caps) {
        kl_sock_close(sockets, prep.fd);
        dg->last_error = KL_ERR_UNSUPPORTED;
        return -1;
    }

    /* Sizing (§12.3) — explicit units, documented defaults. */
    int slot_policy = (cfg->queue_policy == KL_DATAGRAM_QUEUE_SLOT);
    size_t slot_cap = cfg->send_slot_cap ? cfg->send_slot_cap : 65507u;
    size_t budget   = slot_policy ? 0u : (cfg->send_byte_budget ? cfg->send_byte_budget : (256u * 1024u));
    size_t slots    = cfg->send_slots;
    if (slots == 0) {
        if (slot_policy) slots = 8u;                       /* exact frozen SLOT default */
        else { slots = budget / slot_cap; if (slots == 0) slots = 1u; }
    }
    int want_mcast = (cfg->multicast_group && cfg->multicast_group[0]) ? 1 : 0;

    KlDatagramConfig dc; memset(&dc, 0, sizeof(dc));
    dc.ctx = cfg->ctx; dc.alloc = alloc; dc.sockets = sockets; dc.fd = prep.fd;
    dc.send_slots = slots; dc.send_slot_cap = slot_cap; dc.recv_cap = recv_cap;
    dc.want_caps = cfg->want_caps | (want_mcast ? (unsigned)KL_DGRAM_CAP_MULTICAST : 0u);
    dc.optional_caps = cfg->optional_caps | (unsigned)KL_DGRAM_CAP_CONNECTED;   /* always opportunistic (P1-B) */
    dc.accepted_rx_caps = prep.rx_caps;
    if (kl_datagram_init_ex(dg, &dc, budget) != 0) {
        /* init failed BEFORE adoption → the fd was not adopted (caller retains it). init_ex already set
         * dg->last_error + memset dg; close the prepared fd exactly once. */
        kl_sock_close(sockets, prep.fd);
        return -1;
    }
    /* fd is now adopted by dg; any failure below tears down THROUGH KlDatagram (which closes it once). */

    /* Optional multicast join-at-init (post-adoption). CAP_MULTICAST is mandatory (added above), so the
     * grant is present where supported; a join failure (bad group / syscall) tears down. */
    if (want_mcast &&
        kl_datagram_multicast_join(dg, cfg->multicast_group, cfg->multicast_iface) != 0) {
        KlError e = kl_datagram_last_error(dg);
        kl_datagram_teardown(dg, NULL, NULL);   /* closes the fd exactly once; memsets dg */
        dg->last_error = e;                     /* re-surface the cause on the reclaimed handle */
        return -1;
    }
    return 0;
}

/* D1: provider-neutral connect (first-time only). Granted-cap gated (P1-B); reconnect unsupported (P1-C);
 * connected state lives in the core send machine so single/batch/GSO all enforce it (P1-2); refused once
 * close has begun (P1-3). */
int kl_datagram_connect(KlDatagram *dg, const KlSockAddr *peer) {
    if (!dg || !dg->core || !kl_handle_valid(dg->fd) || !peer) {
        if (dg) dg->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    /* P1-3: only an OPEN datagram may connect. Once close has begun the core + fd are still live, so
     * without this a closing/closed datagram could issue a connect syscall — refuse without one. */
    if (kl_dgram_core_close_state(dg->core) != KL_DGRAM_CLOSE_OPEN) {
        dg->last_error = KL_ERR_INVALID_ARG; return -1;
    }
    if (kl_dgram_core_connected(dg->core)) {   /* reconnect is UNSUPPORTED in D1 — refuse without a syscall */
        dg->last_error = KL_ERR_INVALID_ARG; return -1;
    }
    if (!(kl_datagram_caps(dg) & KL_DGRAM_CAP_CONNECTED)) {   /* the GRANTED cap, consistent with the send gate */
        dg->last_error = KL_ERR_UNSUPPORTED; return -1;
    }
    if (kl_sock_connect(dg->sockets, dg->fd, peer) < 0) {
        dg->last_error = KL_ERR_CONNECT;   /* first-connect failure: stays unconnected + usable */
        return -1;
    }
    kl_dgram_core_set_connected(dg->core, 1);   /* now peerless sends (single/batch/GSO) are admitted */
    return 0;
}

KlDatagramSendStatus kl_datagram_send(KlDatagram *dg, const KlDatagramMessage *m) {
    if (!dg || !dg->core) return KL_DATAGRAM_ERROR;
    /* D1: a peerless (connected-mode) send requires the granted CAP_CONNECTED AND an ACTUAL successful
     * connect — enforced uniformly in the core send machine's admission (send_validate), so the single,
     * batch, and GSO paths share the one rule (O-D6-3: KL_DATAGRAM_UNSUPPORTED covers both "not granted"
     * and "not connected"). No separate facade gate here. */
    KlDatagramSendStatus st = kl_dgram_core_send(dg->core, m);
    dg_reconcile_write(dg);   /* a queued (would-block) readiness send needs WRITE interest */
    return st;
}

/* ── M5.2a batch send ─────────────────────────────────────────────────────────────────────────── */

static const KlSockAddr *dg_addr_or_null(const KlSockAddr *a) {
    return (a && kl_sockaddr_family(a) != KL_AF_UNSPEC) ? a : NULL;
}

/* Wired as the send machine's on_drop: a recoverable batch datagram was dropped (a hard send error) —
 * record it so kl_datagram_last_error / the dropped count surface it. Fired from ANY drain path (the
 * batch flush, or the ordinary writable-edge single-flight when a retained bad slot reaches the head),
 * so a bad-middle datagram is reported wherever it is finally submitted. */
static void dg_on_send_drop(void *ctx) {
    KlDatagram *dg = ctx;
    dg->dropped++;
    dg->last_error = KL_ERR_IO;
}

struct dg_batch_submit_ctx { KlDatagram *dg; KlDatagramBatch *b; };

/* KlDgramSubmitBatchFn: one sendmmsg when the tx block is present, else a portable single-send loop.
 * Classifies a -1 into WOULD_BLOCK vs a hard error via kl_sock_io_status BEFORE returning (immediately,
 * before another provider call can overwrite it) so the send machine stays transport-neutral. */
static KlDgramBatchResult dg_batch_submit(void *ctx, const KlDgramTxDesc *descs, int n, int *sent) {
    struct dg_batch_submit_ctx *c = ctx;
    KlDatagram *dg = c->dg;
    const KlDatagramOps *ops = c->b->ops;
    void *pc = dg->sockets ? dg->sockets->context : NULL;
    *sent = 0;
    if (c->b->tx_block && ops && ops->send_batch) {
        int rc = ops->send_batch(pc, dg->fd, c->b->tx_block, descs, n);   /* one sendmmsg */
        if (rc >= 0) { *sent = rc; return KL_DGRAM_BATCH_SENT; }
        return (kl_sock_io_status(dg->sockets) == KL_IO_WOULD_BLOCK)
               ? KL_DGRAM_BATCH_WOULDBLOCK : KL_DGRAM_BATCH_ERROR;
    }
    for (int i = 0; i < n; i++) {   /* portable fallback: one provider send() per descriptor, FIFO order */
        kl_ssize_t r = ops->send(pc, dg->fd, descs[i].data, descs[i].len,
                                 dg_addr_or_null(&descs[i].dest), dg_addr_or_null(&descs[i].src), descs[i].tos);
        if (r >= 0) { (*sent)++; continue; }
        if (i == 0)
            return (kl_sock_io_status(dg->sockets) == KL_IO_WOULD_BLOCK)
                   ? KL_DGRAM_BATCH_WOULDBLOCK : KL_DGRAM_BATCH_ERROR;
        return KL_DGRAM_BATCH_SENT;   /* a prefix went out; stop the run here (retain the rest) */
    }
    return KL_DGRAM_BATCH_SENT;
}

int kl_datagram_send_batch(KlDatagram *dg, KlDatagramBatch *b, const KlDgramTxDesc *descs, int n,
                           KlDatagramSendStatus *stop) {
    if (!dg || !dg->core || !b || n < 0 || (n > 0 && !descs)) {
        if (stop) *stop = KL_DATAGRAM_ERROR;
        return -1;
    }
    if (b->owner != dg || !(b->dir & KL_DGRAM_BATCH_SEND)) {   /* wrong owner / not a send batch */
        if (stop) *stop = KL_DATAGRAM_ERROR;
        return -1;
    }
    KlDgramCore *core = dg->core;
    KlDgramSend *snd = &core->send;

    /* Hold ONE busy frame across the WHOLE operation. The drain's final activity release can otherwise
     * run a deferred teardown that frees dg/core; bracketing defers that terminal to dispatch_end, which
     * is the LAST access to dg/core here (only the local `accepted` is touched after). */
    kl_dgram_core_dispatch_begin(core);

    /* Admit an accepted prefix (enqueue-only — no per-datagram submit, no fast path). */
    KlDatagramSendStatus st = KL_DATAGRAM_ACCEPTED;
    int accepted = 0;
    for (; accepted < n; accepted++) {
        KlDatagramMessage m; memset(&m, 0, sizeof(m));
        m.data = descs[accepted].data; m.len = descs[accepted].len; m.tos = descs[accepted].tos;
        m.peer  = dg_addr_or_null(&descs[accepted].dest);
        m.local = dg_addr_or_null(&descs[accepted].src);
        st = kl_dgram_send_enqueue(snd, &m);
        if (st != KL_DATAGRAM_ACCEPTED) break;
    }
    if (stop) *stop = st;   /* ACCEPTED iff all n taken, else the first refusal (descs[accepted]) */

    /* One drain attempt (§4.2 — one batch attempt per call): the readiness batch-drain over the head-
     * run, or the completion single-flight pump. A retained remainder (backpressure) arms WRITE and
     * drains via the ordinary writable-edge single-flight path — a retained batch slot keeps its
     * recoverable provenance there (its slot flag), and dropped datagrams are reported via on_drop
     * (dg_on_send_drop) regardless of which path submits them. */
    if (dg->completion) {
        (void)kl_dgram_send_flush(snd);
    } else {
        struct dg_batch_submit_ctx sctx = { dg, b };
        (void)kl_dgram_send_flush_batch(snd, b->tx_descs, b->n_slots, dg_batch_submit, &sctx);
    }
    dg_reconcile_write(dg);   /* arm WRITE if anything is still queued (as the single send path does) */

    kl_dgram_core_dispatch_end(core);   /* LAST access to dg/core — may run the deferred teardown/free */
    return accepted;                    /* a local — safe even if dg/core were just freed */
}

/* ── M5.2b GSO ────────────────────────────────────────────────────────────────────────────────── */

/* KlDgramSubmitGsoFn: one whole-buffer provider send_gso (readiness). GSO carries no per-packet TOS
 * (the public API has none) — the socket default applies; `owner`/`tos` are unused here. */
static KlDgramSubmitResult dg_submit_gso(void *ctx, void *owner, const void *buf, size_t total,
                                         size_t seg, const KlSockAddr *peer, int tos) {
    (void)owner; (void)tos;
    KlDatagram *dg = ctx;
    const KlDatagramOps *ops = (dg->sockets && dg->sockets->dgram) ? dg->sockets->dgram : kl_sockdef_dgram();
    void *pc = dg->sockets ? dg->sockets->context : NULL;
    if (!ops->send_gso) return KL_DGRAM_SUBMIT_UNSUPPORTED;
    kl_ssize_t r = ops->send_gso(pc, dg->fd, buf, total, (uint16_t)seg, dg_addr_or_null(peer));
    if (r >= 0) return KL_DGRAM_SUBMIT_DONE;
    KlIoStatus st = kl_sock_io_status(dg->sockets);
    if (st == KL_IO_WOULD_BLOCK)  return KL_DGRAM_SUBMIT_WOULDBLOCK;
    if (st == KL_IO_UNSUPPORTED) { dg->core->gso_unsupported = 1; return KL_DGRAM_SUBMIT_UNSUPPORTED; }
    return KL_DGRAM_SUBMIT_ERROR;
}

/* A GSO group's last segment retired → the batch group buffer is free (clears gso_busy). */
static void dg_on_gso_done(void *ctx, void *owner) {
    (void)ctx;
    if (owner) ((KlDatagramBatch *)owner)->gso_busy = 0;
}

KlDatagramSendStatus kl_datagram_send_gso(KlDatagram *dg, KlDatagramBatch *b, const void *buf,
                                          size_t total_len, uint16_t segment_size, const KlSockAddr *peer) {
    if (!dg || !dg->core || !b) return KL_DATAGRAM_ERROR;
    if (b->owner != dg || !(b->dir & KL_DGRAM_BATCH_SEND)) return KL_DATAGRAM_ERROR;
    if (segment_size == 0) return KL_DATAGRAM_ERROR;
    if (total_len > 0 && !buf) return KL_DATAGRAM_ERROR;
    if (total_len > SIZE_MAX - (size_t)(segment_size - 1)) return KL_DATAGRAM_ERROR;   /* nseg overflow */
    size_t nseg = (total_len + segment_size - 1) / segment_size;
    if (nseg == 0) nseg = 1;   /* 0-length → one empty datagram */

    KlDgramCore *core = dg->core;
    if ((size_t)segment_size > core->out.out_cap) return KL_DATAGRAM_TOO_LARGE;   /* seg ≤ send_slot_cap */
    if (total_len > b->gso_bytes)                  return KL_DATAGRAM_TOO_LARGE;   /* group buffer capacity */
    if (b->gso_busy)                               return KL_DATAGRAM_WOULD_BLOCK;/* one group per batch */

    if (total_len) memcpy(b->gso_buf, buf, total_len);   /* copy once into the group buffer */
    /* GSO one-syscall only on a readiness datagram with a working, un-latched provider send_gso; else the
     * group is FALLBACK (its segments drain as ordinary sends). */
    int fallback = dg->completion || core->gso_unsupported || !(dg->provider_caps & KL_DGRAM_CAP_GSO);

    kl_dgram_core_dispatch_begin(core);   /* bracket the drain (may run a deferred teardown, P1-1) */
    KlDatagramSendStatus st = kl_dgram_send_enqueue_gso(&core->send, b->gso_buf, total_len, segment_size,
                                                        nseg, dg_addr_or_null(peer), -1, fallback, b);
    if (st == KL_DATAGRAM_ACCEPTED) {
        b->gso_busy = 1;   /* the group references b->gso_buf until it retires (on_gso_done clears it) */
        if (dg->completion) {
            (void)kl_dgram_send_flush(&core->send);
        } else {
            struct dg_batch_submit_ctx sctx = { dg, b };
            (void)kl_dgram_send_flush_batch(&core->send, b->tx_descs, b->n_slots, dg_batch_submit, &sctx);
        }
        dg_reconcile_write(dg);
    }
    kl_dgram_core_dispatch_end(core);   /* LAST access to dg/core */
    return st;
}

int kl_datagram_gso_active(const KlDatagram *dg) {
    if (!dg || !dg->core) return 0;
    if (dg->completion)                              return 0;   /* one-syscall GSO is readiness-only */
    if (dg->core->gso_unsupported)                   return 0;   /* latched to per-segment fallback */
    if (!(dg->provider_caps & KL_DGRAM_CAP_GSO))     return 0;   /* provider has no GSO */
    const KlDatagramOps *ops = (dg->sockets && dg->sockets->dgram) ? dg->sockets->dgram : kl_sockdef_dgram();
    return ops->send_gso != NULL;                                /* a real one-syscall op exists */
}

int kl_datagram_recv_attach_batch(KlDatagram *dg, KlDatagramBatch *b) {
    if (!dg || !dg->core || !b) return -1;
    if (dg->completion) return -1;                       /* readiness-only (D-M5-3) */
    if (b->owner != dg || !(b->dir & KL_DGRAM_BATCH_RECV)) return -1;   /* wrong owner / not a recv batch */
    if (dg->core->ext || b->attached) return -1;         /* re-attach */
    if (kl_dgram_core_recv_started(dg->core)) return -1; /* must attach BEFORE recv_start (explicit sentinel) */
    /* Consume b: the core now owns it (freed at the destructive tail). The readiness recv machine draws
     * logical datagrams from its cursor via dg_rdy_pull_batch. GRO split activates only under the §6.2
     * two-part gate: provider CAP_GRO AND the socket's accepted RX_GRO mask (from the M0 prep). */
    b->attached = 1; b->n_filled = 0; b->cursor_i = 0; b->seg_off = 0;
    b->gro_active = (dg->provider_caps & KL_DGRAM_CAP_GRO) &&
                    (kl_dgram_core_accepted_rx_caps(dg->core) & KL_DGRAM_RX_GRO);
    dg->core->ext = b;
    kl_dgram_core_set_view_pull(dg->core, dg_rdy_pull_batch, dg);
    return 0;
}

void kl_datagram_recv_segments(KlDatagram *dg, KlDatagramRecvSegmentsFn cb, void *ud) {
    if (!dg) return;
    dg->on_recv_segments = cb; dg->recv_seg_ud = ud;
}

/* M6.0a: the TOS/Traffic-Class byte of the datagram currently being delivered (read the inbound slot,
 * which the recv paths stamp before each delivery), or -1 if unavailable (RX_TOS not enabled, or none
 * present). Only meaningful inside the on_recv / on_recv_segments callback. */
int kl_datagram_recv_tos(const KlDatagram *dg) {
    if (!dg || !dg->core) return -1;
    KlDgramSlot *in = kl_dgram_core_inbound_slot(dg->core);
    return in ? in->tos : -1;
}

int kl_datagram_recv_start(KlDatagram *dg, KlDatagramRecvFn on_recv, void *ud) {
    if (!dg || !dg->core) return -1;
    /* P1-A fail-loud guard: a socket with GRO capture ENABLED (accepted RX_GRO) must have an
     * actively-splitting RECV batch attached, or a coalesced buffer would be delivered UNSPLIT as one
     * datagram (the M5.4 boundary-corruption bug). Refuse recv_start rather than deliver corrupt data —
     * GRO-without-splitting is thus unrepresentable. (An attached-but-inactive batch, gro_active == 0,
     * counts as no splitter.) */
    if (kl_dgram_core_accepted_rx_caps(dg->core) & KL_DGRAM_RX_GRO) {
        const KlDatagramBatch *b = (const KlDatagramBatch *)dg->core->ext;
        if (!b || !b->gro_active) { dg->last_error = KL_ERR_UNSUPPORTED; return -1; }
    }
    dg->on_recv = on_recv; dg->recv_ud = ud;
    return kl_dgram_core_recv_start(dg->core);   /* the machine drives arm → dg_rdy_arm sets interest */
}
void kl_datagram_pause(KlDatagram *dg)  { if (dg && dg->core) kl_dgram_core_pause(dg->core); }
int  kl_datagram_resume(KlDatagram *dg) { return (dg && dg->core) ? kl_dgram_core_resume(dg->core) : -1; }
void kl_datagram_recv_stop(KlDatagram *dg) { if (dg && dg->core) kl_dgram_core_recv_stop(dg->core); }

int kl_datagram_close_begin(KlDatagram *dg)  { return (dg && dg->core) ? kl_dgram_core_close_begin(dg->core)  : -1; }
int kl_datagram_close_cancel(KlDatagram *dg) { return (dg && dg->core) ? kl_dgram_core_close_cancel(dg->core) : -1; }

int kl_datagram_free(KlDatagram *dg) {
    if (!dg) return -1;
    if (!dg->core) return 0;   /* already reclaimed (e.g. after teardown) → idempotent no-op success */
    if (kl_dgram_core_close_state(dg->core) != KL_DGRAM_CLOSE_CLOSED) { dg->last_error = KL_ERR_INVALID_ARG; return -1; }
    KlAllocator *a = dg->alloc; KlDgramCore *core = dg->core;
    /* M5.3: reclaim the core-adopted RECV batch (§5.4 destructive tail) BEFORE kl_dgram_core_free (which
     * memsets the core, zeroing core->ext). State is CLOSED, so the recv machine no longer reads the
     * ext's rx storage. */
    if (core->ext) { KlDatagramBatch *xb = (KlDatagramBatch *)core->ext; core->ext = NULL; xb->reclaim(xb); }
    if (kl_dgram_core_free(core) != 0) return -1;
    kl_free(a, core, sizeof(*core));
    memset(dg, 0, sizeof(*dg));   /* fully-detached — reusable (invariant 8) */
    return 0;
}

/* The abandon reclaim (DESTRUCTIVE TAIL of the silent terminal, §4a) — frees the object-owned machines +
 * this heap core + memsets the handle, then invokes the owner reclaim to free the embedding container.
 * Runs synchronously (no busy frame) or at the outermost leave (deferred); nothing accesses the core or
 * the machines after this returns. */
static void dg_abandon_reclaim(void *ctx) {
    KlDatagram *dg = ctx;
    KlAllocator *a = dg->alloc;
    KlDgramCore *core = dg->core;
    /* Capture the owner reclaim BEFORE freeing the core (its source). Scope cannot be narrowed — it spans
     * the free below. cppcheck-suppress variableScope */
    KlDatagramReclaimFn owner = (KlDatagramReclaimFn)core->abandon_owner;
    /* cppcheck-suppress variableScope */
    void               *owner_ctx = core->abandon_owner_ctx;
    kl_dgram_core_free_object_owned(core);   /* close + send(abandon) + slots — NOT the rx holder */
    if (core->ext) { KlDatagramBatch *xb = (KlDatagramBatch *)core->ext; core->ext = NULL; xb->reclaim(xb); }  /* M5.3 */
    kl_free(a, core, sizeof(*core));         /* free the heap core block */
    memset(dg, 0, sizeof(*dg));              /* fully-detached — reusable */
    if (owner) owner(owner_ctx);             /* free the embedding owner (e.g. the resolver) — last */
}

/* Internal (src/datagram_open.h): SYNCHRONOUS owner-destruction teardown (Option A + §4a). Arms the
 * silent-abandon terminal on the core; the close coordinator's busy handshake runs it immediately (no
 * active frame) or DEFERS it to the outermost leave (invoked from within a delivery frame) — so it is
 * reentrancy-safe. Idempotent. See datagram_open.h for the full contract. */
int kl_datagram_teardown(KlDatagram *dg, KlDatagramReclaimFn reclaim, void *reclaim_ctx) {
    if (!dg) return -1;
    if (!dg->core) return 0;   /* already reclaimed → idempotent no-op; reclaim NOT invoked */
    return kl_dgram_core_abandon(dg->core, dg_abandon_reclaim, dg, (void (*)(void *))reclaim, reclaim_ctx);
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
unsigned kl_datagram_provider_caps(const KlDatagram *dg) { return dg ? dg->provider_caps : 0; }
/* D1: the KL_DGRAM_RX_* capture mask the socket actually accepted (M0 prep) — complements want_rx_caps. */
unsigned kl_datagram_accepted_rx_caps(const KlDatagram *dg) {
    return (dg && dg->core) ? kl_dgram_core_accepted_rx_caps(dg->core) : 0u;
}

/* M2 extended-UDP layer: runtime multicast join/leave, gated on KL_DGRAM_CAP_MULTICAST, routed to the
 * provider's mcast_membership. Deterministic error precedence (docs/datagram_m2_capability_design.md §3):
 * missing capability → KL_ERR_UNSUPPORTED (checked first, no provider call); malformed group literal →
 * KL_ERR_INVALID_ARG (no provider call); provider/syscall failure → KL_ERR_IO. */
static int dg_multicast(KlDatagram *dg, const char *group, unsigned iface_index, int join) {
    if (!dg || !dg->core || !group) { if (dg) dg->last_error = KL_ERR_INVALID_ARG; return -1; }
    const KlDatagramOps *ops = dg_ops(dg);
    if (!(dg->provider_caps & KL_DGRAM_CAP_MULTICAST) || !ops || !ops->mcast_membership) {
        dg->last_error = KL_ERR_UNSUPPORTED; return -1;
    }
    /* Family from the group literal (KlDatagramConfig carries none; §D-M2-8). A literal that is not a
     * numeric MULTICAST address (malformed, or a valid non-multicast IP) is a caller error. */
    KlSockAddr ga;
    if (kl_sockaddr_parse(&ga, group, 0) != 0 || !kl_sockaddr_is_multicast(&ga)) {
        dg->last_error = KL_ERR_INVALID_ARG; return -1;
    }
    int family = (kl_sockaddr_family(&ga) == KL_AF_INET6) ? AF_INET6 : AF_INET;
    if (ops->mcast_membership(dg_sp_ctx(dg), dg->fd, family, group, iface_index, join) != 0) {
        dg->last_error = KL_ERR_IO; return -1;
    }
    return 0;
}
/* M6.0a: socket-default outgoing TOS/Traffic-Class (IP_TOS / IPV6_TCLASS), the socket-wide default
 * applied to every send that carries no per-message tos — distinct from the per-message KlDatagramMessage.
 * tos. Routed to the provider's set_tos op; the family is derived from the adopted fd (getsockname), so
 * KlDatagram need not store it. Error precedence: bad handle/arg → KL_ERR_INVALID_ARG; TOS capability
 * absent OR no provider op → KL_ERR_UNSUPPORTED (no provider call); undeterminable/non-IP family →
 * KL_ERR_INVALID_ARG (no provider call); provider/syscall failure → KL_ERR_SOCKET. */
int kl_datagram_set_tos(KlDatagram *dg, int tos) {
    if (!dg || !dg->core || !kl_handle_valid(dg->fd) || tos < 0 || tos > 255) {
        if (dg) dg->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    const KlDatagramOps *ops = dg_ops(dg);
    /* Capability truthfulness (M2 exact-fd contract): require BOTH the op AND the per-fd/family TOS
     * capability. A reduced provider may expose set_tos while correctly omitting KL_DGRAM_CAP_TOS for
     * this fd — fail UNSUPPORTED without invoking the provider. */
    if (!ops || !ops->set_tos || !(dg->provider_caps & KL_DGRAM_CAP_TOS)) {
        dg->last_error = KL_ERR_UNSUPPORTED; return -1;
    }
    KlSockAddr la;
    if (kl_sock_get_local_addr(dg->sockets, dg->fd, &la) != 0) { dg->last_error = KL_ERR_SOCKET; return -1; }
    /* Switch explicitly — NEVER guess a family. An unspecified/non-IP/malformed local address must fail
     * without calling set_tos (else the wrong socket option level could be applied). */
    int family;
    switch (kl_sockaddr_family(&la)) {
        case KL_AF_INET:  family = AF_INET;  break;
        case KL_AF_INET6: family = AF_INET6; break;
        default: dg->last_error = KL_ERR_INVALID_ARG; return -1;
    }
    if (ops->set_tos(dg_sp_ctx(dg), dg->fd, family, tos) != 0) { dg->last_error = KL_ERR_SOCKET; return -1; }
    return 0;
}

int kl_datagram_multicast_join(KlDatagram *dg, const char *group, unsigned iface_index) {
    return dg_multicast(dg, group, iface_index, 1);
}
int kl_datagram_multicast_leave(KlDatagram *dg, const char *group, unsigned iface_index) {
    return dg_multicast(dg, group, iface_index, 0);
}
KlDgramCloseState kl_datagram_close_state(const KlDatagram *dg) {
    return (dg && dg->core) ? kl_dgram_core_close_state(dg->core) : KL_DGRAM_CLOSE_CLOSED;
}
KlDatagramCloseResult kl_datagram_close_result(const KlDatagram *dg) {
    return (dg && dg->core) ? kl_dgram_core_close_result(dg->core) : KL_DGRAM_CLOSE_NONE;
}
size_t kl_datagram_send_queued(const KlDatagram *dg)   { return (dg && dg->core) ? dg_send_queued(dg)   : 0; }
size_t kl_datagram_send_inflight(const KlDatagram *dg) { return (dg && dg->core) ? dg_send_inflight(dg) : 0; }
/* M6.0a: the BYTE view of the send backlog (queued + in-flight payload), for byte-based backpressure
 * reporting. Distinct from the slot COUNT (kl_datagram_send_queued). */
size_t kl_datagram_send_queued_bytes(const KlDatagram *dg) {
    return (dg && dg->core) ? kl_dgram_send_queued_bytes(&dg->core->send) : 0;
}
uint64_t kl_datagram_dropped(const KlDatagram *dg)     { return dg ? dg->dropped   : 0; }
uint64_t kl_datagram_truncated(const KlDatagram *dg)   { return dg ? dg->truncated : 0; }
KlError  kl_datagram_last_error(const KlDatagram *dg)  { return dg ? dg->last_error : KL_ERR_INVALID_ARG; }
/* Only report the fd/port once it is ADOPTED (a live core) and the handle is valid — never a
 * zeroed-but-not-inited handle, nor a stale fd after a failed init or after teardown (core == NULL). */
KlSocketHandle kl_datagram_fd(const KlDatagram *dg) {
    return (dg && dg->core && kl_handle_valid(dg->fd)) ? dg->fd : KL_INVALID_SOCKET;
}
uint16_t kl_datagram_local_port(const KlDatagram *dg) {
    if (!dg || !dg->core || !kl_handle_valid(dg->fd)) return 0;
    KlSockAddr la;
    if (kl_sock_get_local_addr(dg->sockets, dg->fd, &la) != 0) return 0;
    return kl_sockaddr_port(&la);
}
