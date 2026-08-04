/*
 * lwip_raw_glue.c — the lwIP-touching half of the Phase 9 raw completion backend.
 *
 * This TU includes ONLY lwIP's NO_SYS=1 raw headers — never KEEL's socket/net headers —
 * so lwIP's htons/ntohs macros and its ssize_t typedef cannot clash with the host
 * <sys/socket.h>/<netinet/in.h> that event_lwip_raw.c pulls in through the KEEL socket
 * seam. The two halves meet across lwip_raw_glue.h (opaque void* pcb/netif/lwrctx + neutral
 * KlLwrRecord; no lwIP type escapes). See docs/phase9_lwip_raw_design.md.
 *
 * ── Stage A rewrite (this file) ──────────────────────────────────────────────
 * A per-context/per-connection data model that fixes four coupled correctness bugs:
 *
 *   #1 receive truncation / ack-before-deliver. The old recv callback copied each pbuf into
 *      a fixed 8 KiB stage buffer, DROPPED the overflow, and immediately tcp_recved()'d the
 *      WHOLE datagram — acking bytes it never delivered. Now each slot owns a bounded RETAINED
 *      pbuf chain (real lwIP flow control): the recv callback either RETAINS the pbuf (no
 *      copy, no ack) or, at the per-conn bound, returns ERR_MEM WITHOUT freeing (lwIP holds it
 *      and re-delivers = backpressure). tcp_recved() is issued ONLY as bytes are copied out
 *      into Keel's read_buf during the drain. No byte is ever acked before it is delivered,
 *      and none is silently dropped.
 *
 *   #2 arm-capacity mismatch. The backend had a separate 8-entry armed table smaller than the
 *      glue's 32-slot conn table, itself smaller than Keel's max_connections (256). Now arm
 *      state lives in the per-conn slot (`armed`), and the slot table is sized to conn_cap =
 *      KlConfig.max_connections — ONE authoritative limit. A conn that has a slot always arms.
 *
 *   #4 silent completion-ring drop. A fixed 64-entry global ring (lwr_rec_push) silently
 *      dropped completions when full — a lost terminal = a leaked/never-closed KlConn. Gone:
 *      each slot carries its own pending-completion flags (accept / write / terminal); the
 *      drain SCANS all slots and emits one completion per pending item, bounded by conn_cap.
 *      It cannot overflow, and a terminal is a per-slot flag -> always deliverable.
 *
 *   #5 global mutable state. g_conns[]/g_listen_pcb/g_recs are replaced by an opaque
 *      per-context KlLwrCtx (allocated through KlAllocator at ctx create, not in a callback or
 *      hot path). The lwIP CORE (lwip_init) stays a process-global one-time init; a single
 *      file-scope "active ctx" guard enforces the NO_SYS=1 single-stack invariant (rejecting a
 *      2nd simultaneous ctx) and is the ONLY legitimate global — sequential create/destroy/
 *      create works. The raw-API TEST CLIENT moved out to lwip_raw_testclient.c.
 *
 * The send/file path (Stage B) is preserved verbatim except its per-conn fields were relocated
 * into the new slot and its helpers now take the ctx — no send logic changed.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lwip_raw_glue.h"

#include "lwip/init.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/sys.h"
#include "lwip/pbuf.h"

#include <keel/allocator.h>   /* KlAllocator + kl_malloc/kl_free — the NEW Stage-A state.
                               * Only KlAllocator (a plain vtable of fn ptrs + sizes) crosses;
                               * it pulls NO socket/net type, so the lwIP-header seam holds. */

#include <string.h>
#include <time.h>
#include <stdlib.h>   /* malloc/free for the EXISTING per-pcb send buffer (Stage B — the send
                       * path is not redesigned here; its heap use is unchanged) */
#include <stdint.h>   /* SIZE_MAX (sendfile size overflow guard) */
#include <unistd.h>   /* pread + off_t for the file-send path */

#ifndef NDEBUG
#include <assert.h>   /* debug-only impossible-state assertions */
#endif

/* ── NO_SYS=1 requires the port to supply sys_now() (u32 ms, monotonic) ──────────
 * lwIP calls this from sys_check_timeouts(). Lives here (the lwIP-only TU) so it links with
 * liblwip_raw.a. */
#define KL_LWR_NS_PER_MS 1000000L
u32_t sys_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u32_t)((u32_t)ts.tv_sec * 1000u + (u32_t)(ts.tv_nsec / KL_LWR_NS_PER_MS));
}

/* ── per-connection receive bound (fix #1) ───────────────────────────────────────
 * Each slot retains at most KL_LWR_RX_MAX bytes of un-delivered received data in its pbuf
 * chain. At the bound the recv callback returns ERR_MEM (lwIP retains + re-delivers = real
 * backpressure). 64 KiB comfortably spans a large header block or a body chunk while bounding
 * per-conn memory. PER-BACKEND receive memory bound = conn_cap * KL_LWR_RX_MAX (the send
 * buffer is a separate, response-sized Stage-B allocation). Overridable at build time
 * (-DKL_LWR_RX_MAX=...) so a small-config test can shrink it below TCP_WND to deterministically
 * exercise the ERR_MEM backpressure path. */
#ifndef KL_LWR_RX_MAX
#define KL_LWR_RX_MAX (64u * 1024u)
#endif

/* tcp_write's u16_t len ceiling (Stage B send-pump chunking). */
#define KL_LWR_TCP_WRITE_MAX 0xffffu

/* ── per-connection slot ─────────────────────────────────────────────────────────
 * One slot per accepted connection; the slot array is sized to conn_cap (== max_connections).
 * A free slot has pcb == NULL. A `dead` slot carries the (freed-by-lwIP) pointer for
 * owner/close correlation only — callers must consult ->dead before dereferencing ->pcb. */
typedef struct {
    struct tcp_pcb *pcb;        /* NULL = free slot */
    void           *owner;      /* KlConn* the backend associated (tcp_arg) */

    /* ── retained receive queue (fix #1) ──────────────────────────────────────────
     * rx_head is a retained pbuf chain (oldest first, appended with pbuf_cat). We OWN it and
     * dequeue bytes from its front as we deliver them (pbuf_free_header). rx_queued = total
     * retained (un-delivered) bytes — the KL_LWR_RX_MAX bound + the has-data status. */
    struct pbuf    *rx_head;
    size_t          rx_queued;

    int             armed;      /* a recv is posted (single in-flight recv per conn) */
    int             closed;     /* peer closed / errored — surface a terminal after rx drains */
    int             dead;       /* pcb freed by lwIP (tcp_err) — ->pcb is NULL, use ->dead_fd */
    void           *dead_fd;    /* the (freed) pcb pointer, kept ONLY for the backend's close
                                 * correlation (c->fd). NOT used to reach lwIP. Because ->pcb is
                                 * cleared to NULL when the slot dies, lwr_conn_find (keyed on the
                                 * LIVE ->pcb) can NEVER alias a reused pcb address to a dead slot
                                 * — the new accept that reuses the address gets its own slot and
                                 * its callbacks resolve to it, not this corpse. */
    int             terminated; /* a terminal completion was already surfaced — no double close */

    /* ── per-slot pending completions (fix #4 — replaces the global ring) ──────────
     * Each is at most 1 pending; the drain scans slots and emits one completion per set flag,
     * clearing it. Bounded by conn_cap -> cannot overflow. */
    int             pend_accept;      /* ACCEPT waiting to be surfaced */
    uint8_t         peer_ip[4];       /* ACCEPT peer IPv4 (network order) */
    uint16_t        peer_port;        /* ACCEPT peer port (host order) */
    int             pend_write;       /* a completed WRITE waiting to be surfaced */
    size_t          pend_write_bytes; /* bytes acked for the pending WRITE */
    int             pend_terminal;    /* a terminal (ok=0) completion waiting to be surfaced */

    /* ── Stage B outgoing send state (owned copy of the full response) — relocated ──
     * Unchanged logic: send_buf/send_total hold the whole payload; send_written = handed to
     * tcp_write; send_acked = peer-acked. When send_acked == send_total the terminal WRITE is
     * surfaced (pend_write) and the buffer released. send_buf == NULL = no send in flight. */
    unsigned char  *send_buf;
    size_t          send_total;
    size_t          send_written;
    size_t          send_acked;
} KlLwrConn;

/* ── per-context backend state (fix #5 — de-globalize) ────────────────────────────
 * Everything that used to be file-scope now lives here, allocated through KlAllocator at ctx
 * create. */
typedef struct KlLwrCtx {
    KlAllocator    *alloc;
    struct tcp_pcb *listen_pcb;   /* the relocated LISTEN pcb (tcp_listen result), or NULL */
    struct netif   *loopif;       /* the loopback netif */
    KlLwrConn      *conns;        /* conn_cap slots (kl_malloc'd) */
    int             conn_cap;
} KlLwrCtx;

/* ── lwIP core init + single-active-ctx guard (the ONE legitimate global) ─────────
 * NO_SYS=1: lwip_init() sets up the single loop netif + one timer wheel = process-global core
 * state. So only ONE raw ctx can be live at a time. `g_active_ctx` enforces that: a second
 * concurrent kl_lwr_ctx_create is rejected; it clears on destroy so sequential
 * create/destroy/create works. lwip_init runs exactly once (the core persists across ctxs).
 *
 * The callbacks recover the ctx via g_active_ctx (safe: exactly one ctx is live, and the
 * callbacks fire inline on that ctx's tick — NO_SYS=1 single-thread, no locks). */
static int       g_lwip_inited = 0;
static KlLwrCtx *g_active_ctx = NULL;

static KlLwrCtx *lwr_ctx(void) { return g_active_ctx; }

void *kl_lwr_active_ctx(void) { return g_active_ctx; }

/* Find a slot by its LIVE pcb pointer. A dead slot has ->pcb == NULL, so it NEVER matches — this
 * is what makes the raw callbacks (recv/sent/err resolve the owning slot from tpcb) safe against
 * lwIP reusing a freed pcb address for a NEW accept: the new conn's callbacks resolve to the new
 * slot, never to the corpse of the conn that previously held that address. */
static KlLwrConn *lwr_conn_find(KlLwrCtx *ctx, const struct tcp_pcb *pcb) {
    if (!ctx || pcb == NULL) return NULL;
    for (int i = 0; i < ctx->conn_cap; i++)
        if (ctx->conns[i].pcb == pcb) return &ctx->conns[i];
    return NULL;
}

/* Find a slot by the backend's fd handle: a LIVE slot whose ->pcb matches, OR a DEAD slot whose
 * ->dead_fd matches (the freed pointer the backend still holds in c->fd). Used ONLY by the
 * backend-facing close path (kl_lwr_tcp_close), which must correlate a dead conn's c->fd to its
 * slot to clear it without dereferencing the freed pcb. A LIVE match is preferred (checked
 * first), so a reused address resolves to the live conn — never the corpse. */
static KlLwrConn *lwr_slot_by_fd(KlLwrCtx *ctx, const void *fd) {
    if (!ctx || fd == NULL) return NULL;
    KlLwrConn *dead = NULL;
    for (int i = 0; i < ctx->conn_cap; i++) {
        if (ctx->conns[i].pcb == (const struct tcp_pcb *)fd) return &ctx->conns[i];  /* live */
        if (ctx->conns[i].dead && ctx->conns[i].dead_fd == fd) dead = &ctx->conns[i];
    }
    return dead;
}

/* Free the whole retained rx pbuf chain (cancellation / close / destroy — no leak). */
static void lwr_rx_free(KlLwrConn *c) {
    if (c->rx_head) { pbuf_free(c->rx_head); c->rx_head = NULL; }
    c->rx_queued = 0;
}

static void lwr_send_reset(KlLwrConn *c) {
    if (c->send_buf) { free(c->send_buf); c->send_buf = NULL; }
    c->send_total = c->send_written = c->send_acked = 0;
}

/* Reserve a free slot for a freshly accepted pcb (zero-initialised). Returns NULL if full
 * (the accept path then tcp_abort's the new pcb — never represented). */
static KlLwrConn *lwr_conn_alloc(KlLwrCtx *ctx, struct tcp_pcb *pcb) {
    for (int i = 0; i < ctx->conn_cap; i++)
        if (ctx->conns[i].pcb == NULL && !ctx->conns[i].dead) {
            memset(&ctx->conns[i], 0, sizeof(ctx->conns[i]));
            ctx->conns[i].pcb = pcb;
            return &ctx->conns[i];
        }
    return NULL;
}

/* Fully release a slot back to free (pcb==NULL) after releasing owned resources (rx chain +
 * send buffer). Recycling: a freed slot is immediately reusable by the next accept, and its
 * NULL pcb can never alias a new pcb's pointer. */
static void lwr_slot_clear(KlLwrConn *c) {
    lwr_rx_free(c);
    lwr_send_reset(c);
    memset(c, 0, sizeof(*c));   /* pcb=NULL + all flags/pending cleared */
}

/* Mark the slot's single terminal completion pending (exactly-once). Sets `terminated` so the
 * armed-READ terminal gate cannot ALSO surface a terminal for the same conn. Only when not
 * already terminated and an owner is known (the backend needs a target). */
static void lwr_mark_terminal(KlLwrConn *c) {
    if (!c || c->terminated || c->owner == NULL) return;
    c->pend_terminal = 1;
    c->terminated = 1;
}

/* ── ctx lifecycle ───────────────────────────────────────────────────────────── */

/* Bring up NO_SYS=1 lwIP (once) + find/configure the loopback netif. Returns the loop netif
 * or NULL. Separated from per-ctx state: the lwIP core is process-global. */
static struct netif *lwr_lwip_core_up(void) {
    if (!g_lwip_inited) {
        lwip_init();          /* auto-creates the loop netif via LWIP_HAVE_LOOPIF */
        g_lwip_inited = 1;
    }
    struct netif *lo = NULL, *nif;
    NETIF_FOREACH(nif) {
        if (ip4_addr_isloopback(netif_ip4_addr(nif))) { lo = nif; break; }
    }
    if (!lo) return NULL;

    ip4_addr_t ip, nm, gw;
    IP4_ADDR(&ip, 127, 0, 0, 1);
    IP4_ADDR(&nm, 255, 0, 0, 0);
    IP4_ADDR(&gw, 127, 0, 0, 1);
    netif_set_addr(lo, &ip, &nm, &gw);
    netif_set_up(lo);
    netif_set_link_up(lo);
    netif_set_default(lo);
    return lo;
}

void *kl_lwr_ctx_create(void *alloc_v, int conn_cap) {
    KlAllocator *alloc = alloc_v;   /* the neutral seam carries the allocator as void* */
    if (!alloc || conn_cap <= 0) return NULL;
    if (g_active_ctx != NULL) return NULL;   /* NO_SYS=1: one raw stack at a time (reject 2nd) */

    struct netif *lo = lwr_lwip_core_up();
    if (!lo) return NULL;

    KlLwrCtx *ctx = kl_malloc(alloc, sizeof(*ctx));
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof(*ctx));
    ctx->alloc = alloc;
    ctx->loopif = lo;
    ctx->conn_cap = conn_cap;

    /* Overflow-safe slot-array size: conn_cap * sizeof(KlLwrConn). */
    if ((size_t)conn_cap > SIZE_MAX / sizeof(KlLwrConn)) {
        kl_free(alloc, ctx, sizeof(*ctx));
        return NULL;
    }
    size_t bytes = (size_t)conn_cap * sizeof(KlLwrConn);
    ctx->conns = kl_malloc(alloc, bytes);
    if (!ctx->conns) {
        kl_free(alloc, ctx, sizeof(*ctx));
        return NULL;
    }
    memset(ctx->conns, 0, bytes);

    g_active_ctx = ctx;
    return ctx;
}

void *kl_lwr_ctx_loopif(void *lwrctx) {
    KlLwrCtx *ctx = lwrctx;
    return ctx ? ctx->loopif : NULL;
}

int kl_lwr_ctx_ensure_cap(void *lwrctx, int conn_cap) {
    KlLwrCtx *ctx = lwrctx;
    if (!ctx || conn_cap <= 0) return -1;
    if (conn_cap <= ctx->conn_cap) return 0;   /* already large enough */
    if ((size_t)conn_cap > SIZE_MAX / sizeof(KlLwrConn)) return -1;
    size_t nbytes = (size_t)conn_cap * sizeof(KlLwrConn);
    size_t obytes = (size_t)ctx->conn_cap * sizeof(KlLwrConn);
    /* Grow before any accept — no live slots to relocate; a fresh array is simplest + correct. */
    KlLwrConn *nc = kl_realloc(ctx->alloc, ctx->conns, obytes, nbytes);
    if (!nc) return -1;
    memset((char *)nc + obytes, 0, nbytes - obytes);   /* zero the new tail */
    ctx->conns = nc;
    ctx->conn_cap = conn_cap;
    return 0;
}

void kl_lwr_ctx_destroy(void *lwrctx) {
    KlLwrCtx *ctx = lwrctx;
    if (!ctx) return;

    /* Abort any live connection pcb + free its rx chain + send buffer. */
    for (int i = 0; i < ctx->conn_cap; i++) {
        KlLwrConn *c = &ctx->conns[i];
        if (c->pcb == NULL) continue;
        struct tcp_pcb *p = c->pcb;
        int dead = c->dead;
        lwr_slot_clear(c);           /* frees rx chain + send buffer, clears the slot */
        if (!dead) {                 /* live pcb — detach callbacks then abort (frees it) */
            tcp_arg(p, NULL);
            if (p->state != LISTEN) {
                tcp_recv(p, NULL);
                tcp_sent(p, NULL);
                tcp_err(p, NULL);
            }
            tcp_abort(p);
        }
    }
    /* Close the listener. */
    if (ctx->listen_pcb) {
        struct tcp_pcb *lp = ctx->listen_pcb;
        ctx->listen_pcb = NULL;
        tcp_arg(lp, NULL);
        tcp_accept(lp, NULL);
        if (tcp_close(lp) != ERR_OK) tcp_abort(lp);
    }

    size_t bytes = (size_t)ctx->conn_cap * sizeof(KlLwrConn);
    KlAllocator *alloc = ctx->alloc;
    kl_free(alloc, ctx->conns, bytes);
    if (g_active_ctx == ctx) g_active_ctx = NULL;   /* clear the guard — sequential create OK */
    kl_free(alloc, ctx, sizeof(*ctx));
}

void kl_lwr_lwip_tick(void *loopif) {
    sys_check_timeouts();                          /* fire lwIP timers (retransmit, etc.) */
    if (loopif) netif_poll((struct netif *)loopif); /* drain loopback TX → RX */
}

/* ── raw tcp_* callbacks → per-slot state ─────────────────────────────────────── */

/* recv callback for an accepted (server-side) connection. A NULL pbuf / err = peer closed.
 * FLOW CONTROL (fix #1):
 *   - NULL p or err: mark closed; do NOT free a null p; keep the pcb (driver closes it). If a
 *     send is in flight (close-with-outstanding: client read part of a big body then FIN'd),
 *     the conn is NOT recv-armed, so surface a terminal now + drop the send buffer.
 *   - at the per-conn bound (rx_queued + p->tot_len > KL_LWR_RX_MAX): return ERR_MEM WITHOUT
 *     freeing/queuing/acking p — lwIP retains p and re-delivers later (real backpressure).
 *   - else RETAIN p: append to rx_head (pbuf_cat, refcount-aware — NO copy, NO pbuf_free), add
 *     to rx_queued, return ERR_OK. We now OWN the chain and free it as we consume it.
 * tcp_recved() is NEVER called here — only as bytes are copied out in kl_lwr_take_staged. */
static err_t lwr_srv_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)arg;
    KlLwrCtx *ctx = lwr_ctx();
    KlLwrConn *cs = lwr_conn_find(ctx, tpcb);

    if (err != ERR_OK || p == NULL) {
        if (p) pbuf_free(p);
        if (cs) {
            cs->closed = 1;
            if (cs->send_buf) {          /* close-with-outstanding: tear down promptly */
                lwr_send_reset(cs);
                lwr_mark_terminal(cs);
            }
        }
        return ERR_OK;   /* keep the pcb; the driver's close tears it down */
    }

    if (!cs) {                            /* no slot for this pcb — cannot receive; drop cleanly.
                                           * (Should not happen: every accepted pcb has a slot.) */
        pbuf_free(p);
        return ERR_OK;
    }

    /* Bound check with overflow guard (rx_queued + tot_len must not exceed KL_LWR_RX_MAX). */
    if (p->tot_len > KL_LWR_RX_MAX ||
        cs->rx_queued > (size_t)KL_LWR_RX_MAX - p->tot_len) {
        /* At the bound: backpressure. Retain p in lwIP (return ERR_MEM, no free, no ack). */
        return ERR_MEM;
    }

    /* Retain the pbuf chain (refcount-aware append; no copy). */
    if (cs->rx_head == NULL) {
        cs->rx_head = p;
    } else {
        pbuf_cat(cs->rx_head, p);   /* transfers p's ref into the existing chain */
    }
    cs->rx_queued += p->tot_len;
    return ERR_OK;
}

/* ── Stage B send-pump (unchanged logic; relocated fields + ctx-scoped find) ────── */
static void lwr_send_pump(struct tcp_pcb *pcb, KlLwrConn *cs) {
    if (!cs || !cs->send_buf) return;
    int wrote_any = 0;
    while (cs->send_written < cs->send_total) {
        u16_t sndbuf = tcp_sndbuf(pcb);
        if (sndbuf == 0) break;                       /* no headroom — wait for tcp_sent */
        size_t remain = cs->send_total - cs->send_written;
        size_t chunk = remain;
        if (chunk > sndbuf) chunk = sndbuf;
        if (chunk > KL_LWR_TCP_WRITE_MAX) chunk = KL_LWR_TCP_WRITE_MAX;
        err_t w = tcp_write(pcb, cs->send_buf + cs->send_written, (u16_t)chunk,
                            TCP_WRITE_FLAG_COPY);
        if (w == ERR_MEM) break;                      /* queue full — backpressure, resume later */
        if (w != ERR_OK) { tcp_abort(pcb); return; }  /* hard error — abort the conn */
        cs->send_written += chunk;
        wrote_any = 1;
    }
    if (wrote_any) tcp_output(pcb);                   /* flush the freshly-queued segments */
}

/* sent callback: `len` bytes acked. Advance acked, pump more, and only when the WHOLE payload
 * is acknowledged mark the single terminal WRITE completion pending + release the send buffer. */
static err_t lwr_srv_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    (void)arg;
    KlLwrCtx *ctx = lwr_ctx();
    KlLwrConn *cs = lwr_conn_find(ctx, tpcb);
    if (!cs || !cs->send_buf) return ERR_OK;          /* stray ack (no send in flight) */
    cs->send_acked += len;
    if (cs->send_acked >= cs->send_total) {
        cs->pend_write = 1;
        cs->pend_write_bytes = cs->send_total;
        lwr_send_reset(cs);                           /* release the owned copy */
        return ERR_OK;
    }
    lwr_send_pump(tpcb, cs);                          /* push more of the tail */
    return ERR_OK;
}

/* err callback: the pcb was aborted by the stack (RST/OOM/self-initiated). lwIP has ALREADY
 * FREED the pcb — we MUST NOT dereference it. Key the slot by owner (arg == the KlConn* set via
 * tcp_arg), free its rx chain + send buffer BY OWNER, mark it dead + closed, and surface the
 * single terminal completion. */
static void lwr_srv_err(void *arg, err_t err) {
    (void)err;
    KlLwrCtx *ctx = lwr_ctx();
    if (!ctx) return;
    for (int i = 0; i < ctx->conn_cap; i++) {
        KlLwrConn *c = &ctx->conns[i];
        if (c->owner == arg && c->pcb != NULL) {
            lwr_rx_free(c);                /* free-by-owner: pcb is dead, can't reach the chain */
            lwr_send_reset(c);
            c->dead_fd = c->pcb;           /* keep the freed pointer for close correlation only */
            c->pcb = NULL;                 /* clear the LIVE handle → find() can't alias a reuse */
            c->dead = 1;
            c->closed = 1;
            lwr_mark_terminal(c);
            break;
        }
    }
}

/* accept callback: reserve a slot, arm the conn callbacks, mark ACCEPT pending. If the slot
 * table is full (accepts beyond conn_cap), tcp_abort the new pcb — rejected, never represented
 * (mirrors the completion contract: no accepted-but-unrepresentable pcb). */
static err_t lwr_srv_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    KlLwrCtx *ctx = arg;   /* the listener's tcp_arg is the ctx */
    if (err != ERR_OK || newpcb == NULL) return ERR_VAL;
    if (!ctx) { tcp_abort(newpcb); return ERR_ABRT; }

    KlLwrConn *cs = lwr_conn_alloc(ctx, newpcb);
    if (!cs) { tcp_abort(newpcb); return ERR_ABRT; }   /* full — reject */

    tcp_recv(newpcb, lwr_srv_recv);
    tcp_sent(newpcb, lwr_srv_sent);
    tcp_err(newpcb, lwr_srv_err);

    cs->pend_accept = 1;
    memcpy(cs->peer_ip, &newpcb->remote_ip.addr, 4);   /* network order (ip4 addr) */
    cs->peer_port = newpcb->remote_port;                /* host order in lwIP */
    return ERR_OK;
}

/* ── socket-provider primitives on tcp_pcb ─────────────────────────────────── */

void *kl_lwr_tcp_new(void) {
    return tcp_new();
}

int kl_lwr_tcp_bind(void *pcb, const uint8_t ip4[4], uint16_t port) {
    ip_addr_t addr;
    if (!ip4 || (ip4[0] | ip4[1] | ip4[2] | ip4[3]) == 0) {
        addr = *IP_ADDR_ANY;
    } else {
        IP_ADDR4(&addr, ip4[0], ip4[1], ip4[2], ip4[3]);
    }
    return tcp_bind((struct tcp_pcb *)pcb, &addr, port) == ERR_OK ? 0 : -1;
}

void *kl_lwr_tcp_listen(void *lwrctx, void *pcb) {
    KlLwrCtx *ctx = lwrctx;
    struct tcp_pcb *lp = tcp_listen((struct tcp_pcb *)pcb);
    if (!lp) return NULL;
    tcp_arg(lp, ctx);                 /* the accept callback receives the ctx as arg */
    tcp_accept(lp, lwr_srv_accept);
    if (ctx) ctx->listen_pcb = lp;    /* the original `pcb` is now freed by lwIP */
    return lp;
}

void *kl_lwr_listen_pcb(void *lwrctx) {
    KlLwrCtx *ctx = lwrctx;
    return ctx ? ctx->listen_pcb : NULL;
}

uint16_t kl_lwr_tcp_local_port(void *pcb) {
    return ((struct tcp_pcb *)pcb)->local_port;
}

void kl_lwr_tcp_close(void *lwrctx, void *pcb) {
    KlLwrCtx *ctx = lwrctx;
    struct tcp_pcb *p = (struct tcp_pcb *)pcb;
    if (p == NULL) return;

    /* Resolve the slot by fd handle: a LIVE match (->pcb == p) is preferred; otherwise a DEAD
     * slot whose ->dead_fd == p (tcp_err already freed the pcb — the backend still holds the old
     * pointer in c->fd). For a dead slot we ONLY clear the slot — never dereference the freed pcb
     * (no UAF on close-after-err). A reused address resolves to the LIVE conn (checked first), so
     * closing a dead fd can never tear down a fresh conn that happens to reuse the address. */
    KlLwrConn *slot = lwr_slot_by_fd(ctx, p);
    if (slot && slot->dead) {
        lwr_slot_clear(slot);
        return;
    }

    /* The listener: close it directly (it is not tracked in the conn slots). */
    if (ctx && p == ctx->listen_pcb) {
        ctx->listen_pcb = NULL;
        tcp_arg(p, NULL);
        tcp_accept(p, NULL);
        if (tcp_close(p) != ERR_OK) tcp_abort(p);
        return;
    }

    /* A connection pcb: close it ONLY if we still track it via a LIVE slot. If there is no slot,
     * this fd was already torn down (its slot cleared) — closing again would re-enter tcp_close on
     * an already-closed/TIME-WAIT pcb and corrupt lwIP's TCP lists (the "TIME-WAIT pcb->state"
     * assertion). So an untracked close is an idempotent no-op — the exactly-once close discipline
     * lives in the slot lifetime, not in repeated tcp_close calls. */
    if (!slot) return;

    lwr_slot_clear(slot);             /* frees rx chain + owned send buffer, clears the slot */
    tcp_arg(p, NULL);
    if (p->state != LISTEN) {         /* connection pcb: detach the conn callbacks */
        tcp_recv(p, NULL);
        tcp_sent(p, NULL);
        tcp_err(p, NULL);
    }
    /* tcp_close may fail (data still queued) — lwIP REQUIRES a tcp_abort fallback then. */
    if (tcp_close(p) != ERR_OK)
        tcp_abort(p);
}

void kl_lwr_tcp_abort(void *lwrctx, void *pcb) {
    KlLwrCtx *ctx = lwrctx;
    struct tcp_pcb *p = (struct tcp_pcb *)pcb;
    if (p == NULL || (ctx && p == ctx->listen_pcb)) return;
    KlLwrConn *slot = lwr_conn_find(ctx, p);
    if (!slot || slot->dead) return;   /* already gone — idempotent no-op */

    /* Release owned resources + surface the single terminal. Keep slot->owner + a dead_fd copy
     * of the (about-to-be-freed) pcb for the backend's close correlation; clear slot->pcb so
     * find() can't alias the address once lwIP reuses it. The subsequent close finds the dead
     * slot by dead_fd and clears it. */
    lwr_rx_free(slot);
    lwr_send_reset(slot);
    slot->closed = 1;
    lwr_mark_terminal(slot);
    slot->dead_fd = slot->pcb;
    slot->pcb = NULL;
    slot->dead = 1;                    /* pcb about to be freed — never deref again */

    tcp_arg(p, NULL);
    if (p->state != LISTEN) {
        tcp_recv(p, NULL);
        tcp_sent(p, NULL);
        tcp_err(p, NULL);
    }
    tcp_abort(p);                      /* frees the pcb + RST */
}

void kl_lwr_set_owner(void *lwrctx, void *pcb, void *owner) {
    KlLwrCtx *ctx = lwrctx;
    struct tcp_pcb *p = (struct tcp_pcb *)pcb;
    KlLwrConn *cs = lwr_conn_find(ctx, p);
    if (cs) cs->owner = owner;
    tcp_arg(p, owner);   /* the err callback receives this as arg (owner-keyed teardown) */
}

int kl_lwr_conn_arm(void *lwrctx, void *pcb) {
    KlLwrCtx *ctx = lwrctx;
    KlLwrConn *cs = lwr_conn_find(ctx, (struct tcp_pcb *)pcb);
    if (!cs) return -1;   /* no slot — the backend must not leave this conn accepted-but-mute */
    cs->armed = 1;
    return 0;
}

void kl_lwr_conn_disarm(void *lwrctx, void *pcb) {
    KlLwrCtx *ctx = lwrctx;
    KlLwrConn *cs = lwr_conn_find(ctx, (struct tcp_pcb *)pcb);
    if (cs) cs->armed = 0;
}

void kl_lwr_conn_status(void *lwrctx, void *pcb, int *has_data, int *closed) {
    KlLwrCtx *ctx = lwrctx;
    KlLwrConn *cs = lwr_conn_find(ctx, (struct tcp_pcb *)pcb);
    /* A dead (tcp_err-freed) slot never has usable data — only the terminal. `terminated`
     * suppresses re-reporting once the backend consumed the terminal READ (defence against a
     * double comp_close). */
    if (has_data) *has_data = (cs && !cs->dead && !cs->terminated && cs->rx_queued > 0) ? 1 : 0;
    if (closed)   *closed   = (cs && !cs->terminated && cs->closed) ? 1 : 0;
}

int kl_lwr_next_readable(void *lwrctx, int *cursor, void **owner, void **pcb, int *closed) {
    KlLwrCtx *ctx = lwrctx;
    if (!ctx || !cursor) return 0;
    for (int i = *cursor; i < ctx->conn_cap; i++) {
        KlLwrConn *c = &ctx->conns[i];
        if (c->owner == NULL || !c->armed) continue;
        int has_data = (!c->dead && !c->terminated && c->rx_queued > 0);
        int is_closed = (!c->terminated && c->closed);
        if (!has_data && !is_closed) continue;
        *cursor = i + 1;   /* advance past this slot for the next call */
        if (owner)  *owner  = c->owner;
        if (pcb)    *pcb    = c->pcb;
        if (closed) *closed = (!has_data && is_closed) ? 1 : 0;
        return 1;
    }
    *cursor = ctx->conn_cap;
    return 0;
}

void kl_lwr_mark_terminated(void *lwrctx, void *pcb) {
    KlLwrCtx *ctx = lwrctx;
    KlLwrConn *cs = lwr_conn_find(ctx, (struct tcp_pcb *)pcb);
    if (cs) cs->terminated = 1;
}

/* Copy up to `cap` received bytes from the retained pbuf chain into `dst`, then dequeue exactly
 * that many bytes from the FRONT of the chain and ack them. Uses lwIP's own chain primitives so
 * pbuf refcounts / tot_len / the pool free-list stay consistent (a hand-rolled per-pbuf detach
 * is fragile — freeing individual segments of a pbuf_cat'ed chain can corrupt the pool):
 *   - pbuf_copy_partial(rx_head, dst, n, 0)  — copy the first n bytes across pbuf boundaries.
 *   - pbuf_free_header(rx_head, n)           — remove n bytes from the front, freeing fully-
 *                                              consumed head pbufs and returning the new head
 *                                              (a partial head is kept, payload/len adjusted).
 *   - tcp_recved(pcb, n)                     — ack EXACTLY the delivered bytes (fix #1: never
 *                                              ack a byte before it is delivered into read_buf).
 * `cap` is the free room in read_buf (<= read_cap <= header window), which fits u16_t. */
size_t kl_lwr_take_staged(void *lwrctx, void *pcb, void *dst, size_t cap) {
    KlLwrCtx *ctx = lwrctx;
    struct tcp_pcb *p = (struct tcp_pcb *)pcb;
    KlLwrConn *cs = lwr_conn_find(ctx, p);
    if (!cs || cs->dead || cs->rx_head == NULL || cs->rx_queued == 0 || cap == 0) return 0;

    size_t want = cs->rx_queued < cap ? cs->rx_queued : cap;
    if (want > 0xffffu) want = 0xffffu;                 /* pbuf_copy_partial/free_header u16 len */
    u16_t n = (u16_t)want;

    u16_t got = pbuf_copy_partial(cs->rx_head, dst, n, 0);
    if (got == 0) return 0;

    cs->rx_head = pbuf_free_header(cs->rx_head, got);   /* dequeue got bytes from the front */
    cs->rx_queued -= got;
    if (cs->rx_queued == 0) cs->rx_head = NULL;         /* fully drained (defensive) */
    if (p) tcp_recved(p, got);                          /* ack ONLY the delivered bytes */
    return got;
}

/* ── Stage B: full-payload send + file send (owned copy + pump) — ctx-threaded ──── */

static int lwr_send_install(KlLwrCtx *ctx, struct tcp_pcb *p, unsigned char *buf, size_t total) {
    KlLwrConn *cs = lwr_conn_find(ctx, p);
    if (!cs) { free(buf); return -1; }
    lwr_send_reset(cs);              /* one send in flight per conn (completion contract) */
    cs->send_buf     = buf;
    cs->send_total   = total;
    cs->send_written = 0;
    cs->send_acked   = 0;
    if (total == 0) {               /* nothing to send — synthesize an immediate completion */
        cs->pend_write = 1;
        cs->pend_write_bytes = 0;
        lwr_send_reset(cs);
        return 0;
    }
    lwr_send_pump(p, cs);
    return 0;
}

int kl_lwr_send_begin(void *lwrctx, void *pcb, const void *buf, size_t len) {
    KlLwrCtx *ctx = lwrctx;
    struct tcp_pcb *p = (struct tcp_pcb *)pcb;
    unsigned char *copy = malloc(len ? len : 1);
    if (!copy) return -1;
    if (len) memcpy(copy, buf, len);
    return lwr_send_install(ctx, p, copy, len);
}

int kl_lwr_sendfile_begin(void *lwrctx, void *pcb, const void *head, size_t head_len,
                          int file_fd, uint64_t count) {
    KlLwrCtx *ctx = lwrctx;
    struct tcp_pcb *p = (struct tcp_pcb *)pcb;
    if (count > (uint64_t)(SIZE_MAX - head_len)) return -1;   /* overflow guard */
    size_t total = head_len + (size_t)count;
    unsigned char *copy = malloc(total ? total : 1);
    if (!copy) return -1;
    if (head_len) memcpy(copy, head, head_len);
    size_t off = 0;
    while (off < (size_t)count) {
        ssize_t nr = pread(file_fd, copy + head_len + off, (size_t)count - off, (off_t)off);
        if (nr < 0) { free(copy); return -1; }
        if (nr == 0) break;                       /* unexpected EOF — send what we have */
        off += (size_t)nr;
    }
    if (off != (size_t)count) { free(copy); return -1; }   /* file shorter than declared */
    return lwr_send_install(ctx, p, copy, total);
}

void kl_lwr_send_release(void *lwrctx, void *pcb) {
    KlLwrCtx *ctx = lwrctx;
    KlLwrConn *cs = lwr_conn_find(ctx, (struct tcp_pcb *)pcb);
    if (cs) lwr_send_reset(cs);
}

/* ── drain: scan all slots, emit pending completions (fix #4) ───────────────────
 * Per slot, emit in order: ACCEPT, then WRITE, then terminal. Bounded by conn_cap (`max`
 * caps how many the caller's buffer holds this pass; the rest stay pending for the next
 * drain — nothing is lost). READ is NOT emitted here (surfaced from the rx queue by the
 * backend's armed-conn loop). */
int kl_lwr_drain(void *lwrctx, KlLwrRecord *out, int max) {
    KlLwrCtx *ctx = lwrctx;
    if (!ctx || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < ctx->conn_cap && n < max; i++) {
        KlLwrConn *c = &ctx->conns[i];
        if (c->pcb == NULL && !c->dead) continue;   /* free slot */

        if (c->pend_accept && n < max) {
            KlLwrRecord *r = &out[n++];
            memset(r, 0, sizeof(*r));
            r->kind = KL_LWR_ACCEPT;
            r->accepted = c->pcb;
            r->ok = 1;
            memcpy(r->peer_ip, c->peer_ip, 4);
            r->peer_port = c->peer_port;
            c->pend_accept = 0;
        }
        if (c->pend_write && n < max) {
            KlLwrRecord *r = &out[n++];
            memset(r, 0, sizeof(*r));
            r->kind = KL_LWR_WRITE;
            r->pcb = c->pcb;
            r->owner = c->owner;
            r->nbytes = c->pend_write_bytes;
            r->ok = 1;
            c->pend_write = 0;
            c->pend_write_bytes = 0;
        }
        if (c->pend_terminal && n < max) {
#ifndef NDEBUG
            assert(c->owner != NULL && "terminal completion requires an owner");
#endif
            KlLwrRecord *r = &out[n++];
            memset(r, 0, sizeof(*r));
            r->kind = KL_LWR_WRITE;   /* ok=0 → the driver turns any failed TCP completion into
                                       * comp_close, releasing the KlConn exactly once. */
            r->pcb = c->pcb;
            r->owner = c->owner;
            r->nbytes = 0;
            r->ok = 0;
            c->pend_terminal = 0;
        }
    }
    return n;
}
