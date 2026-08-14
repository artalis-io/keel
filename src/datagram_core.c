/*
 * datagram_core.c — INTERNAL fixed-slot datagram assembly (Phase B, step 7A-3). See datagram_core.h.
 *
 * Wires the built machines into one lifecycle: an OBJECT-owned outbound pool + send machine, a
 * LIFE-token-owned rx holder (inbound + recv, freed by on_final so it outlives a posted op AND the
 * caller-owned core), and the confirmed-detachment close coordinator with the §4.3 retirement seam.
 * Mirrors udp.c's proven rx-lifetime + interest wiring, but over neutral adapters (no provider/KlUdp).
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
    if (core->user_on_close) core->user_on_close(core->user_close_ctx, result);
}

/* ── init / free ──────────────────────────────────────────────────────────────────────────── */

int kl_dgram_core_init(KlDgramCore *core, const KlDgramCoreConfig *cfg) {
    if (!core)
        return -1;
    memset(core, 0, sizeof(*core));
    if (!cfg || !cfg->alloc || !kl_handle_valid(cfg->fd) ||
        cfg->send_slots == 0 || cfg->send_slot_cap == 0 || cfg->recv_cap == 0 ||
        !cfg->submit || !cfg->arm || !cfg->deliver)
        return -1;

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
    KlDgramLife *life = kl_dgram_life_create(a, core, core_rx_final, rx);   /* owner ref (refs=1) */
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
                           cfg->submit, cfg->submit_ctx) != 0) {
        kl_dgram_slots_free(&core->out);
        kl_dgram_life_mark_dead(life); kl_dgram_life_release(life);
        return -1;
    }
    if (kl_dgram_close_init(&core->close, &core->send, &rx->recv, core_on_close, core) != 0) {
        kl_dgram_send_free(&core->send);
        kl_dgram_slots_free(&core->out);
        kl_dgram_life_mark_dead(life); kl_dgram_life_release(life);
        return -1;
    }
    (void)kl_dgram_close_set_cancel(&core->close, cfg->cancel_recv, cfg->cancel_send, cfg->cancel_ctx);
    (void)kl_dgram_close_set_retire(&core->close, cfg->retire, cfg->retire_ctx);

    rx->life           = life;
    core->alloc        = a;
    core->fd           = cfg->fd;     /* fd ADOPTED only now (init succeeded) */
    core->completion   = cfg->completion ? 1 : 0;
    core->caps         = cfg->caps;
    core->rx           = rx;
    core->life         = life;
    core->user_on_close    = cfg->on_close;
    core->user_close_ctx   = cfg->close_ctx;
    core->inited       = 1;
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

KlDgramSlot *kl_dgram_core_inbound_slot(KlDgramCore *core) {
    return (core && core->inited && core->rx) ? kl_dgram_inbound_slot(&core->rx->inbound) : (KlDgramSlot *)0;
}
