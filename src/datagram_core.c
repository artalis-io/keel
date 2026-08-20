/*
 * datagram_core.c — INTERNAL fixed-slot datagram assembly (Phase B, step 7A-3). See datagram_core.h.
 *
 * Wires the built machines into one lifecycle: an OBJECT-owned outbound pool + send machine, a
 * LIFE-token-owned rx holder (inbound + recv, freed by on_final so it outlives a posted op AND the
 * caller-owned core), and the confirmed-detachment close coordinator with the §4.3 retirement seam.
 * Mirrors the proven rx-lifetime + interest wiring over neutral adapters (no provider or transport coupling).
 */
#include "datagram_core.h"

#include <string.h>

/* ── life-owned rx holder ─────────────────────────────────────────────────────────────────── */

/* on_final: the owner ref AND every posted-op ref have been released — the inbound storage is no
 * longer pinned by any op, so free the rx holder (inbound + this). */
static void core_rx_final(void *ctx) {
    KlDgramCoreRx *rx = ctx;
    KlAllocator *a = rx->alloc;
    kl_dgram_inbound_free(&rx->inbound);
    kl_free(a, rx, sizeof(*rx));
}

/* Backend-retirement hook driven ONCE by the close coordinator (after the outbound queue drains): the
 * single point that physically closes the adopted fd. Idempotent by fd_closed as belt-and-suspenders
 * (the coordinator already guarantees once). Runs BEFORE classification/on_close, matching the frozen
 * §4.1 order (fd close is what classifies + retires the still-outstanding recv on a completion backend). */
static void core_backend_close(void *ctx) {
    KlDgramCore *core = ctx;
    if (core->fd_closed) return;
    core->fd_closed = 1;
    if (core->close_transport && kl_handle_valid(core->fd))
        core->close_transport(core->transport_ctx, core->fd);
    core->fd = KL_INVALID_SOCKET;
}

/* The core's own on_close: drop the owner life ref (its final release runs core_rx_final → frees the
 * inbound; on QUARANTINE a posted op still holds a ref → the final release DEFERS, storage pinned),
 * NULL the (now token-owned) rx/life, then forward the terminal classification to the caller. This is
 * a DESTRUCTIVE TAIL — the caller may free the core. */
static void core_on_close(void *ctx, KlDatagramCloseResult result) {
    KlDgramCore *core = ctx;
    KlDgramLife *life = core->life;
    core->rx   = NULL;   /* token-owned now (freed by on_final on final release, or leaked on quarantine) */
    core->life = NULL;
    if (life) {
        kl_dgram_life_mark_dead(life);   /* completions after this see a dead token → dropped */
        kl_dgram_life_release(life);      /* owner ref; runs core_rx_final iff no posted op holds one */
    }
    if (core->abandoning) {
        /* Owner-destruction (§4a): SILENT — do NOT invoke user_on_close (it could free the owner). Run
         * the facade reclaim as the DESTRUCTIVE TAIL: it frees the object-owned machines + this heap
         * core + the handle + the owner. The hook + its ctx are read as call args (before it runs), and
         * nothing accesses `core` after this returns. */
        if (core->abandon_reclaim) core->abandon_reclaim(core->abandon_reclaim_ctx);
        return;
    }
    if (core->user_on_close) core->user_on_close(core->user_close_ctx, result);
}

/* ── init / free ──────────────────────────────────────────────────────────────────────────── */

int kl_dgram_core_init(KlDgramCore *core, const KlDgramCoreConfig *cfg) {
    if (!core)
        return -1;
    memset(core, 0, sizeof(*core));
    if (!cfg || !cfg->alloc || !kl_handle_valid(cfg->fd) ||
        cfg->send_slots == 0 || cfg->send_slot_cap == 0 || cfg->recv_cap == 0 ||
        !cfg->submit || !cfg->arm || !cfg->deliver || !cfg->close_transport)
        return -1;   /* close_transport REQUIRED — the core adopts the fd, so it must close it */

    KlAllocator *a = cfg->alloc;

    /* Life-owned rx holder (inbound + recv), allocated from the event-ctx/backend allocator so it
     * outlives the caller-owned core + any posted op. */
    KlDgramCoreRx *rx = kl_malloc(a, sizeof(*rx));
    if (!rx)
        return -1;
    memset(rx, 0, sizeof(*rx));
    rx->alloc = a;
    if (kl_dgram_inbound_init(&rx->inbound, a, cfg->recv_cap) != 0) {
        kl_free(a, rx, sizeof(*rx));
        return -1;
    }
    if (kl_dgram_recv_init(&rx->recv, &rx->inbound, cfg->completion, cfg->deliver, cfg->deliver_ctx,
                           cfg->arm, cfg->disarm, cfg->pull, cfg->recv_hook_ctx) != 0) {
        kl_dgram_inbound_free(&rx->inbound);
        kl_free(a, rx, sizeof(*rx));
        return -1;
    }
    /* Owner ref (refs=1). `cfg->dispatch` is the completion-routing identity: a live completion facade
     * (7B-3) installs kl_datagram_comp_dispatch so the driver routes this token via life->dispatch;
     * NULL (neutral-adapter tests) means completions are driven directly (kl_dgram_core_*_on_complete). */
    KlDgramLife *life = kl_dgram_life_create(a, core, core_rx_final, rx,
                                             cfg->dispatch);
    if (!life) {
        kl_dgram_inbound_free(&rx->inbound);
        kl_free(a, rx, sizeof(*rx));
        return -1;
    }

    /* Object-owned outbound pool + send machine. */
    if (kl_dgram_slots_init(&core->out, a, cfg->send_slots, cfg->send_slot_cap) != 0) {
        kl_dgram_life_mark_dead(life); kl_dgram_life_release(life);   /* runs core_rx_final */
        return -1;
    }
    if (kl_dgram_send_init(&core->send, &core->out, a, cfg->completion, cfg->caps,
                           cfg->send_byte_budget, cfg->submit, cfg->submit_ctx) != 0) {
        kl_dgram_slots_free(&core->out);
        kl_dgram_life_mark_dead(life); kl_dgram_life_release(life);
        return -1;
    }
    /* D1: initial connected state (0 for a fresh facade datagram — kl_datagram_connect sets it later;
     * non-zero only when adopting an already-connected fd). Governs peerless-send admission uniformly. */
    kl_dgram_send_set_connected(&core->send, cfg->connected);
    if (kl_dgram_close_init(&core->close, &core->send, &rx->recv, core_on_close, core) != 0) {
        kl_dgram_send_free(&core->send);
        kl_dgram_slots_free(&core->out);
        kl_dgram_life_mark_dead(life); kl_dgram_life_release(life);
        return -1;
    }
    (void)kl_dgram_close_set_cancel(&core->close, cfg->cancel_recv, cfg->cancel_send, cfg->cancel_ctx);
    (void)kl_dgram_close_set_retire(&core->close, cfg->retire, cfg->retire_ctx);
    (void)kl_dgram_close_set_backend_close(&core->close, core_backend_close, core);

    /* Final PRE-ADOPTION hook (7B-7) — the LAST fallible step, run only after EVERY allocation above
     * succeeded. On abort, unwind ALL prepared state and return -1 WITHOUT adopting the fd, so a
     * completion facade's fd↔loop registration (kl_event_add) that failed leaves the fd un-associated
     * and re-usable. No allocation follows a successful commit. */
    if (cfg->on_prepared && cfg->on_prepared(cfg->prepared_ctx) != 0) {
        kl_dgram_close_free(&core->close);
        kl_dgram_send_free(&core->send);
        kl_dgram_slots_free(&core->out);
        kl_dgram_life_mark_dead(life); kl_dgram_life_release(life);   /* runs core_rx_final → frees rx */
        return -1;   /* fd NOT adopted; registration (if any) did not commit */
    }

    rx->life           = life;
    core->alloc        = a;
    core->fd           = cfg->fd;     /* fd ADOPTED only now (init succeeded) */
    core->close_transport  = cfg->close_transport;
    core->transport_ctx    = cfg->transport_ctx;
    core->completion   = cfg->completion ? 1 : 0;
    core->caps         = cfg->caps;
    core->accepted_rx_caps = cfg->accepted_rx_caps;   /* M5.1: enabled-per-socket RX mask (facade-masked) */
    core->ext          = NULL;                          /* M5.1: no batch attached */
    core->gso_unsupported  = 0;                         /* M5.1: GSO latch clear */
    core->rx           = rx;
    core->life         = life;
    core->user_on_close    = cfg->on_close;
    core->user_close_ctx   = cfg->close_ctx;
    core->inited       = 1;
    return 0;
}

void kl_dgram_core_dispatch_begin(KlDgramCore *core) {
    if (core && core->inited) kl_dgram_close_hold(&core->close);
}
void kl_dgram_core_dispatch_end(KlDgramCore *core) {
    if (core && core->inited) kl_dgram_close_release(&core->close);   /* may run the terminal + free `core` */
}

void kl_dgram_core_free_object_owned(KlDgramCore *core) {
    if (!core) return;
    /* send_abandon skips the inflight_n guard (safe: dead token + §2.5.1 copy-at-submit); close_free
     * requires CLOSED (the silent abandon set it); slots_free reclaims the whole outbound pool block
     * (incl. any still-occupied in-flight slot — its payload copy lives in the backend op). None of these
     * touch the life-owned rx holder or the heap core block. */
    kl_dgram_close_free(&core->close);
    kl_dgram_send_abandon(&core->send);
    kl_dgram_slots_free(&core->out);
}

int kl_dgram_core_abandon(KlDgramCore *core,
                          void (*reclaim)(void *), void *reclaim_ctx,
                          void (*owner)(void *),   void *owner_ctx) {
    if (!core || !core->inited)
        return -1;
    /* Idempotent while ALREADY abandoning (§4b): a second request during the DEFERRED window — reachable
     * via a nested cancellation callback, not only direct repetition — must be a no-op success that
     * PRESERVES the first request's reclaim/owner callbacks. Overwriting them here would drop (or NULL
     * out → leak) the original owner reclaim before the terminal fires. */
    if (core->abandoning)
        return 0;
    /* Arm the silent-abandon terminal, then request it through the close coordinator. The coordinator's
     * busy handshake decides WHEN it runs: immediately (no active frame) or deferred to the outermost
     * leave (invoked from within a delivery frame). Either way, core_on_close (abandon branch) does
     * mark-dead + owner-ref-release, then runs `reclaim` as the destructive tail. mark_dead/release are
     * NOT done here — they belong in the terminal so they run at the SAME safe point as the free. */
    core->abandoning          = 1;
    core->abandon_reclaim      = reclaim;
    core->abandon_reclaim_ctx  = reclaim_ctx;
    core->abandon_owner        = owner;
    core->abandon_owner_ctx    = owner_ctx;
    kl_dgram_close_abandon(&core->close);   /* silent forced terminal, deferred by the busy handshake */
    return 0;
}

int kl_dgram_core_free(KlDgramCore *core) {
    if (!core)
        return 0;
    if (!core->inited)
        return 0;
    if (kl_dgram_close_state(&core->close) != KL_DGRAM_CLOSE_CLOSED)
        return -1;   /* refuse before a terminal classification (DETACHED/QUARANTINED) */
    /* rx/life already dropped in core_on_close (token-owned now). Free the OBJECT-owned parts. */
    kl_dgram_close_free(&core->close);
    kl_dgram_send_free(&core->send);
    kl_dgram_slots_free(&core->out);
    memset(core, 0, sizeof(*core));   /* reusable */
    return 0;
}

/* ── op forwards ──────────────────────────────────────────────────────────────────────────── */

KlDatagramSendStatus kl_dgram_core_send(KlDgramCore *core, const KlDatagramMessage *m) {
    if (!core || !core->inited) return KL_DATAGRAM_ERROR;
    return kl_dgram_send(&core->send, m);
}
int kl_dgram_core_send_on_complete(KlDgramCore *core, int ok) {
    return (core && core->inited) ? kl_dgram_send_on_complete(&core->send, ok) : -1;
}
int kl_dgram_core_send_flush(KlDgramCore *core) {
    return (core && core->inited) ? kl_dgram_send_flush(&core->send) : -1;
}

int kl_dgram_core_recv_start(KlDgramCore *core) {
    return (core && core->inited && core->rx) ? kl_dgram_recv_start(&core->rx->recv) : -1;
}
int kl_dgram_core_recv_on_complete(KlDgramCore *core, size_t len, int ok) {
    return (core && core->inited && core->rx) ? kl_dgram_recv_on_complete(&core->rx->recv, len, ok) : -1;
}
int kl_dgram_core_recv_on_readable(KlDgramCore *core) {
    return (core && core->inited && core->rx) ? kl_dgram_recv_on_readable(&core->rx->recv) : -1;
}
void kl_dgram_core_pause(KlDgramCore *core) {
    if (core && core->inited && core->rx) kl_dgram_recv_pause(&core->rx->recv);
}
int kl_dgram_core_resume(KlDgramCore *core) {
    return (core && core->inited && core->rx) ? kl_dgram_recv_resume(&core->rx->recv) : -1;
}
void kl_dgram_core_recv_stop(KlDgramCore *core) {
    if (core && core->inited && core->rx) kl_dgram_recv_stop(&core->rx->recv);
}
void kl_dgram_core_set_view_pull(KlDgramCore *core, KlDgramRecvViewFn view_pull, void *ctx) {
    if (core && core->inited && core->rx) kl_dgram_recv_set_view_pull(&core->rx->recv, view_pull, ctx);
}
int kl_dgram_core_recv_started(const KlDgramCore *core) {
    return (core && core->inited && core->rx) ? kl_dgram_recv_started(&core->rx->recv) : 0;
}
int kl_dgram_core_recv_held(const KlDgramCore *core) {
    return (core && core->inited && core->rx) ? kl_dgram_recv_held(&core->rx->recv) : 0;
}

int kl_dgram_core_close_begin(KlDgramCore *core)  { return (core && core->inited) ? kl_dgram_close_begin(&core->close)  : -1; }
int kl_dgram_core_close_cancel(KlDgramCore *core) { return (core && core->inited) ? kl_dgram_close_cancel(&core->close) : -1; }
int kl_dgram_core_close_retry(KlDgramCore *core)  { return (core && core->inited) ? kl_dgram_close_retry(&core->close)  : -1; }

KlDgramCloseState kl_dgram_core_close_state(const KlDgramCore *core) {
    return (core && core->inited) ? kl_dgram_close_state(&core->close) : KL_DGRAM_CLOSE_CLOSED;
}
KlDatagramCloseResult kl_dgram_core_close_result(const KlDgramCore *core) {
    return (core && core->inited) ? kl_dgram_close_result(&core->close) : KL_DGRAM_CLOSE_NONE;
}

void kl_dgram_core_on_writable(KlDgramCore *core, KlDgramWritableFn cb, void *ctx) {
    if (core && core->inited) kl_dgram_send_set_writable_cb(&core->send, cb, ctx);
}
void kl_dgram_core_on_drain(KlDgramCore *core, KlDgramDrainFn cb, void *ctx) {
    if (core && core->inited) kl_dgram_send_set_drain_cb(&core->send, cb, ctx);
}
void kl_dgram_core_on_drop(KlDgramCore *core, KlDgramDropFn cb, void *ctx) {
    if (core && core->inited) kl_dgram_send_set_drop_cb(&core->send, cb, ctx);
}
void kl_dgram_core_set_gso_cbs(KlDgramCore *core, KlDgramSubmitGsoFn submit_gso, void *submit_ctx,
                               KlDgramGsoDoneFn on_gso_done, void *done_ctx) {
    if (core && core->inited)
        kl_dgram_send_set_gso_cbs(&core->send, submit_gso, submit_ctx, on_gso_done, done_ctx);
}
void kl_dgram_core_set_connected(KlDgramCore *core, int on) {   /* D1 */
    if (core && core->inited) kl_dgram_send_set_connected(&core->send, on);
}
int kl_dgram_core_connected(const KlDgramCore *core) {
    return (core && core->inited) ? kl_dgram_send_connected(&core->send) : 0;
}

KlDgramSlot *kl_dgram_core_inbound_slot(KlDgramCore *core) {
    return (core && core->inited && core->rx) ? kl_dgram_inbound_slot(&core->rx->inbound) : (KlDgramSlot *)0;
}

KlDgramLife *kl_dgram_core_life(KlDgramCore *core) {
    return (core && core->inited) ? core->life : (KlDgramLife *)0;
}
