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
 * ── Stage B rewrite (this file) ──────────────────────────────────────────────
 * The send + file-response path is redesigned to use BOUNDED, PREALLOCATED per-connection
 * transmit state — no per-response malloc, no double-copy of the whole payload, no whole-file
 * read into memory:
 *
 *   - A fixed KL_LWR_TX_WIN (32 KiB) staging buffer is kl_malloc'd ONCE PER SLOT at ctx create
 *     (a single conn_cap * KL_LWR_TX_WIN block, sliced per slot). The send path performs ZERO
 *     allocation — no malloc/free/calloc/realloc remains in this TU.
 *
 *   - Buffered response (kl_lwr_send_begin): does NOT copy the whole payload. The slot stores a
 *     bounded COPY of the iov ARRAY (up to KL_LWR_MAX_TX_IOV entries) + total + sent_off. The
 *     iov DATA is referenced in PLACE, relying on the verified lifetime contract: the completion
 *     driver keeps the response's serialized segments (res->hdr_buf, res->body, static status/
 *     keepalive/CRLF literals) valid until the terminal KL_COMP_WRITE (it cannot reset c->res
 *     between kl_comp_post_send and comp_on_write). The ONE exception is the driver's transient
 *     Content-Length scratch (a stack `cl_buf`): any iov segment at or below KL_LWR_TX_SNAP is
 *     eagerly snapshotted into a small preallocated per-slot head buffer at send_begin; larger
 *     segments (the body / big header block) are referenced in place. Bounded, zero-alloc, and
 *     correct regardless of which small segment is transient.
 *
 *   - The PUMP copies the next min(tcp_sndbuf, KL_LWR_TX_WIN, remaining) bytes out of the iov
 *     segments into the preallocated TX window, tcp_write(COPY) + tcp_output, advancing sent_off.
 *     ERR_MEM stops (backpressure); tcp_sent advances send_acked and re-pumps. One terminal WRITE
 *     only when send_acked == total. Transmit memory is bounded by KL_LWR_TX_WIN, INDEPENDENT of
 *     response size — so response size is now unbounded-by-design (the old 16 MiB cap is gone).
 *
 *   - File response (kl_lwr_sendfile_begin): does NOT read the file into memory. The slot stores
 *     the head iov (bounded, snapshotted as above) + file_fd + count + sent_off. The pump sends
 *     the head first, then preads the next <= min(tcp_sndbuf, KL_LWR_TX_WIN) chunk from file_fd
 *     into the TX window and tcp_write(COPY), advancing the file offset; refills on each tcp_sent.
 *     Short reads loop; a pread error surfaces a FAILED terminal WRITE (ok=0) so the driver
 *     closes. The glue only READS file_fd — the response layer owns closing it (no double-close).
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
#include <stdint.h>   /* SIZE_MAX (sendfile size overflow guard) */
#include <unistd.h>   /* pread + off_t for the file-send path */

/* NOTE (Stage B): NO <stdlib.h> — the send path no longer uses malloc/free. All glue-owned
 * memory (the conn-slot array + the per-slot preallocated TX windows/head buffers) is allocated
 * through KlAllocator at ctx create. The send path is provably zero-allocation. */

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

/* ── Stage B: bounded, preallocated per-connection transmit window ────────────────
 * KL_LWR_TX_WIN is the fixed staging buffer each slot pumps THROUGH, allocated ONCE at ctx
 * create (a single conn_cap * KL_LWR_TX_WIN block, sliced per slot). The pump copies at most
 * KL_LWR_TX_WIN bytes per round out of the referenced iov segments (buffered) or pread out of
 * the file (file), tcp_write(COPY)s them, and advances. Because transmit memory is bounded by
 * this window — NOT by the response/file size — a response of ANY size streams in constant
 * memory. PER-BACKEND transmit memory bound = conn_cap * (KL_LWR_TX_WIN + KL_LWR_TX_HEAD)
 * (plus the tiny per-slot iov-array copy). 32 KiB comfortably exceeds a typical MSS-scaled
 * tcp_sndbuf so each pump round fills the send queue in one tcp_write. Overridable at build
 * time (-DKL_LWR_TX_WIN=...) so a small-config test can shrink it to exercise many pump rounds
 * / ERR_MEM backpressure. Overflow-safe: sized against SIZE_MAX in kl_lwr_ctx_create. */
#ifndef KL_LWR_TX_WIN
#define KL_LWR_TX_WIN (32u * 1024u)
#endif

/* Max iov segments a single buffered send may carry (the response iovec is small: status line
 * + header block + Content-Length + keep-alive + CRLF + body = <= 6; file head is <= 5). A send
 * with more segments than this is rejected at send_begin (a documented, never-hit-in-practice
 * limit). Kept in sync with the driver's kl_response_build_iovec cap (7). */
#define KL_LWR_MAX_TX_IOV 8

/* A per-slot preallocated head-snapshot buffer holds a COPY of the small iov segments whose data
 * pointer may be transient (the driver's stack Content-Length scratch `cl_buf`) — any segment at
 * or below KL_LWR_TX_SNAP bytes is snapshotted here at send_begin; larger segments (body / large
 * header block, all owned by the live KlResponse) are referenced in place. Sized to hold every
 * snapshotted segment of one send: KL_LWR_MAX_TX_IOV * KL_LWR_TX_SNAP. */
#define KL_LWR_TX_SNAP 256u
#define KL_LWR_TX_HEAD (KL_LWR_MAX_TX_IOV * KL_LWR_TX_SNAP)

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

    /* ── Stage B outgoing send state (BOUNDED, PREALLOCATED — no whole-payload copy) ──
     * A send references its source in place (the live KlResponse segments) and pumps THROUGH a
     * fixed preallocated window. `send_active` = a send is in flight (replaces the old
     * send_buf!=NULL sentinel). `send_total` = the whole logical payload; `send_off` = bytes
     * handed to tcp_write so far; `send_acked` = peer-acked. When send_acked == send_total the
     * terminal WRITE is surfaced (pend_write). No per-response heap buffer exists.
     *
     * Buffered mode: `iov[]`/`iovcnt` is a bounded COPY of the response iov ARRAY; the DATA is
     * referenced in place (small transient segments were snapshotted into `tx_head`).
     * File mode: `is_file` set, `file_fd`/`file_count` describe the file body that follows the
     * head (head_total bytes of iov). */
    int             send_active;   /* 1 = a send is in flight */
    int             is_file;       /* 1 = file-body send (head iov + file_fd) */
    size_t          send_total;    /* whole logical payload (head + body/file) */
    size_t          send_off;      /* bytes handed to tcp_write */
    size_t          send_acked;    /* bytes peer-acked */

    /* bounded copy of the source iov array (data referenced in place unless snapshotted) */
    struct { const unsigned char *base; size_t len; } iov[KL_LWR_MAX_TX_IOV];
    int             iovcnt;
    size_t          head_total;    /* file mode: total bytes across the head iov */

    int             file_fd;       /* file mode: fd to pread (owned by the response layer) */
    uint64_t        file_count;    /* file mode: bytes to send from file_fd */

    /* tx_head_used tracks bytes packed into this slot's head-snapshot buffer for the current
     * send. The pump window + head-snapshot buffers themselves are NOT cached here — they are
     * derived from the slot index into the ctx's single preallocated TX block (lwr_slot_tx_win /
     * lwr_slot_tx_head), so they survive the memset in lwr_slot_clear / lwr_conn_alloc without a
     * re-bind step. */
    size_t          tx_head_used;
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

    /* ── Stage B: preallocated transmit memory (kl_malloc'd once, sliced per slot) ──
     * ONE contiguous block of conn_cap * KL_LWR_TX_STRIDE bytes; slot i owns bytes
     * [i*STRIDE, i*STRIDE + KL_LWR_TX_WIN) as its pump window and the following KL_LWR_TX_HEAD
     * bytes as its head-snapshot buffer. Never grown per-send; the whole point of Stage B is
     * bounded transmit memory. tx_block_size is kept for the exact kl_free at destroy. */
    unsigned char  *tx_block;
    size_t          tx_block_size;
} KlLwrCtx;

/* Per-slot stride in the ctx TX block: the pump window followed by the head-snapshot buffer. */
#define KL_LWR_TX_STRIDE (KL_LWR_TX_WIN + KL_LWR_TX_HEAD)

/* Slot `c`'s preallocated pump window / head-snapshot buffer, derived from its index into the
 * ctx's single TX block. Index-derived (not cached in the slot) so it survives the memset that
 * lwr_slot_clear / lwr_conn_alloc do. The ctx guarantees c is one of ctx->conns[0..conn_cap). */
static unsigned char *lwr_slot_tx_win(KlLwrCtx *ctx, KlLwrConn *c) {
    size_t idx = (size_t)(c - ctx->conns);
    return ctx->tx_block + idx * KL_LWR_TX_STRIDE;
}
static unsigned char *lwr_slot_tx_head(KlLwrCtx *ctx, KlLwrConn *c) {
    return lwr_slot_tx_win(ctx, c) + KL_LWR_TX_WIN;
}

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

/* Reset the slot's send offsets to "no send in flight". Stage B: nothing to free — the TX
 * window + head-snapshot buffers are preallocated (ctx-owned, index-derived) and reused across
 * sends. A reset slot's send_active is 0; file_fd is set to -1 defensively so no stale fd is
 * ever pread. This is the only send teardown — there is no heap buffer to release. */
static void lwr_send_reset(KlLwrConn *c) {
    c->send_active = 0;
    c->is_file = 0;
    c->send_total = c->send_off = c->send_acked = 0;
    c->iovcnt = 0;
    c->head_total = 0;
    c->file_fd = -1;
    c->file_count = 0;
    c->tx_head_used = 0;
}

/* Reserve a free slot for a freshly accepted pcb (zero-initialised). Returns NULL if full
 * (the accept path then tcp_abort's the new pcb — never represented). */
static KlLwrConn *lwr_conn_alloc(KlLwrCtx *ctx, struct tcp_pcb *pcb) {
    for (int i = 0; i < ctx->conn_cap; i++)
        if (ctx->conns[i].pcb == NULL && !ctx->conns[i].dead) {
            memset(&ctx->conns[i], 0, sizeof(ctx->conns[i]));
            lwr_send_reset(&ctx->conns[i]);   /* normalize send state (file_fd=-1); TX buffers are
                                               * index-derived so the memset did not lose them */
            ctx->conns[i].pcb = pcb;
            return &ctx->conns[i];
        }
    return NULL;
}

/* Fully release a slot back to free (pcb==NULL) after releasing owned resources (the rx pbuf
 * chain; the send state has no heap — the TX window is preallocated). Recycling: a freed slot is
 * immediately reusable by the next accept, and its NULL pcb can never alias a new pcb's pointer.
 * The TX window/head buffers are index-derived (not stored in the slot), so the memset does not
 * lose them. */
static void lwr_slot_clear(KlLwrConn *c) {
    lwr_rx_free(c);
    memset(c, 0, sizeof(*c));   /* pcb=NULL + all flags/pending cleared (send offsets zeroed) */
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

/* Allocate (once) the ctx's preallocated TX block: conn_cap * KL_LWR_TX_STRIDE bytes, sliced
 * per slot into a pump window + head-snapshot buffer. Overflow-safe. Returns 0 / -1. Called at
 * ctx create and re-sized by ensure_cap; the send path never allocates. */
static int lwr_alloc_tx_block(KlLwrCtx *ctx, int conn_cap) {
    if ((size_t)conn_cap > SIZE_MAX / KL_LWR_TX_STRIDE) return -1;   /* overflow guard */
    size_t tx_bytes = (size_t)conn_cap * KL_LWR_TX_STRIDE;
    unsigned char *blk = kl_malloc(ctx->alloc, tx_bytes);
    if (!blk) return -1;
    /* No memset needed: the pump only ever reads bytes it just wrote (send_off/pread), and the
     * head-snapshot buffer is written before read. Left uninitialised deliberately (bounded). */
    ctx->tx_block = blk;
    ctx->tx_block_size = tx_bytes;
    return 0;
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

    /* Preallocate the bounded per-conn transmit block (Stage B — zero send-path alloc). A
     * failure here is a clean init failure (frees the slot array + ctx, leaves the active-ctx
     * guard clear so a retry / sequential create can proceed). */
    if (lwr_alloc_tx_block(ctx, conn_cap) != 0) {
        kl_free(alloc, ctx->conns, bytes);
        kl_free(alloc, ctx, sizeof(*ctx));
        return NULL;
    }

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
    if ((size_t)conn_cap > SIZE_MAX / KL_LWR_TX_STRIDE) return -1;   /* TX-block overflow guard */

    /* Grow the TX block FIRST (before any accept, no live slots to relocate) so that if it
     * fails the slot array is untouched and the ctx stays consistent. */
    size_t ntx = (size_t)conn_cap * KL_LWR_TX_STRIDE;
    unsigned char *ntxb = kl_realloc(ctx->alloc, ctx->tx_block, ctx->tx_block_size, ntx);
    if (!ntxb) return -1;
    ctx->tx_block = ntxb;
    ctx->tx_block_size = ntx;

    size_t nbytes = (size_t)conn_cap * sizeof(KlLwrConn);
    size_t obytes = (size_t)ctx->conn_cap * sizeof(KlLwrConn);
    /* Grow before any accept — no live slots to relocate; a fresh array is simplest + correct. */
    KlLwrConn *nc = kl_realloc(ctx->alloc, ctx->conns, obytes, nbytes);
    if (!nc) return -1;   /* TX block already grown — harmless (larger than needed); ctx usable */
    memset((char *)nc + obytes, 0, nbytes - obytes);   /* zero the new tail */
    ctx->conns = nc;
    ctx->conn_cap = conn_cap;
    return 0;
}

void kl_lwr_ctx_destroy(void *lwrctx) {
    KlLwrCtx *ctx = lwrctx;
    if (!ctx) return;

    /* Abort any live connection pcb + free its rx chain (send state has no heap). */
    for (int i = 0; i < ctx->conn_cap; i++) {
        KlLwrConn *c = &ctx->conns[i];
        if (c->pcb == NULL) continue;
        struct tcp_pcb *p = c->pcb;
        int dead = c->dead;
        lwr_slot_clear(c);           /* frees rx chain, clears the slot */
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
    kl_free(alloc, ctx->tx_block, ctx->tx_block_size);   /* the preallocated Stage-B TX block */
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
            if (cs->send_active) {       /* close-with-outstanding: tear down promptly */
                lwr_send_reset(cs);      /* nothing to free — just drop the in-flight send */
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

/* ── Stage B send-pump: fill the preallocated TX window from the source, then tcp_write ──────
 * Gather `want` bytes into `dst` (the slot's pump window) starting at the logical payload offset
 * `off`:
 *   - buffered: copy from the referenced iov segments (data in place; small transient segments
 *     were snapshotted into tx_head at send_begin, so the stored iov[].base is always valid).
 *   - file: the head bytes come from the iov (as above); bytes at or beyond head_total are
 *     pread from file_fd at (off - head_total). Short reads loop; a read error returns -1.
 * Returns the number of bytes gathered (== want on success), or -1 on a file read error. */
static ssize_t lwr_gather(KlLwrConn *cs, size_t off, unsigned char *dst, size_t want) {
    size_t got = 0;

    /* Head/iov region [0, head_end): head_total for file mode, send_total for buffered mode. */
    size_t head_end = cs->is_file ? cs->head_total : cs->send_total;
    while (got < want && off + got < head_end) {
        /* Locate the iov segment covering logical position (off + got). */
        size_t pos = off + got, base = 0;
        int seg = -1;
        for (int i = 0; i < cs->iovcnt; i++) {
            if (pos < base + cs->iov[i].len) { seg = i; break; }
            base += cs->iov[i].len;
        }
        if (seg < 0) break;   /* defensive: position beyond the iov (should not happen) */
        size_t in_seg = pos - base;
        size_t avail = cs->iov[seg].len - in_seg;
        size_t n = want - got;
        if (n > avail) n = avail;
        if (n > head_end - pos) n = head_end - pos;
        memcpy(dst + got, cs->iov[seg].base + in_seg, n);
        got += n;
    }

    /* File region [head_total, send_total): pread the next chunk. */
    if (cs->is_file) {
        while (got < want && off + got < cs->send_total) {
            size_t fpos = (off + got) - cs->head_total;
            size_t n = want - got;
            ssize_t nr = pread(cs->file_fd, dst + got, n, (off_t)fpos);
            if (nr < 0) return -1;            /* file read error — caller surfaces ok=0 terminal */
            if (nr == 0) break;               /* unexpected EOF — send what we gathered */
            got += (size_t)nr;                /* short read: loop and pread again */
        }
    }
    return (ssize_t)got;
}

/* Pump: hand as many bytes as tcp_sndbuf + KL_LWR_TX_WIN allow into tcp_write, in TX-window-sized
 * rounds, advancing send_off. Stops on ERR_MEM (backpressure — resumed by tcp_sent) or when the
 * whole payload is written. A file read error flags send_failed + aborts the pcb so the terminal
 * WRITE (ok=0) is surfaced and the driver closes. */
static void lwr_send_pump(struct tcp_pcb *pcb, KlLwrConn *cs) {
    KlLwrCtx *ctx = lwr_ctx();
    if (!cs || !cs->send_active || !ctx) return;
    unsigned char *win = lwr_slot_tx_win(ctx, cs);
    int wrote_any = 0;
    while (cs->send_off < cs->send_total) {
        u16_t sndbuf = tcp_sndbuf(pcb);
        if (sndbuf == 0) break;                       /* no headroom — wait for tcp_sent */
        size_t remain = cs->send_total - cs->send_off;
        size_t chunk = remain;
        if (chunk > sndbuf) chunk = sndbuf;
        if (chunk > KL_LWR_TX_WIN) chunk = KL_LWR_TX_WIN;
        if (chunk > KL_LWR_TCP_WRITE_MAX) chunk = KL_LWR_TCP_WRITE_MAX;

        ssize_t got = lwr_gather(cs, cs->send_off, win, chunk);
        /* got < 0 = file read error; got == 0 while bytes remain = truncated file (declared more
         * than the file yields). Either is a mid-transmission failure the client cannot recover
         * from (a short body under a fixed Content-Length): surface a FAILED terminal (ok=0) so
         * the driver closes. We NEVER silently under-deliver a declared-length body. */
        if (got <= 0) {
            lwr_send_reset(cs);                       /* drop the in-flight send (nothing to free) */
            lwr_mark_terminal(cs);                    /* surface a FAILED terminal (ok=0) */
            tcp_abort(pcb);                           /* frees the pcb — err callback keys by owner */
            return;
        }

        err_t w = tcp_write(pcb, win, (u16_t)got, TCP_WRITE_FLAG_COPY);
        if (w == ERR_MEM) break;                      /* queue full — backpressure, resume later */
        if (w != ERR_OK) { tcp_abort(pcb); return; }  /* hard error — abort the conn */
        cs->send_off += (size_t)got;
        wrote_any = 1;
    }
    if (wrote_any) tcp_output(pcb);                   /* flush the freshly-queued segments */
}

/* sent callback: `len` bytes acked. Advance acked, pump more, and only when the WHOLE payload is
 * acknowledged mark the single terminal WRITE completion pending + reset the send offsets (no
 * buffer to free — the TX window is preallocated + reused). */
static err_t lwr_srv_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    (void)arg;
    KlLwrCtx *ctx = lwr_ctx();
    KlLwrConn *cs = lwr_conn_find(ctx, tpcb);
    if (!cs || !cs->send_active) return ERR_OK;       /* stray ack (no send in flight) */
    cs->send_acked += len;
    if (cs->send_acked >= cs->send_total) {
        cs->pend_write = 1;
        cs->pend_write_bytes = cs->send_total;
        lwr_send_reset(cs);                           /* send complete — clear offsets */
        return ERR_OK;
    }
    lwr_send_pump(tpcb, cs);                          /* push more of the tail */
    return ERR_OK;
}

/* err callback: the pcb was aborted by the stack (RST/OOM/self-initiated). lwIP has ALREADY
 * FREED the pcb — we MUST NOT dereference it. Key the slot by owner (arg == the KlConn* set via
 * tcp_arg), free its rx chain + reset its send offsets BY OWNER (nothing to free — the TX window
 * is preallocated), mark it dead + closed, and surface the single terminal completion. */
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

/* ── Stage B: bounded, zero-allocation buffered + file send (reference in place) ─────
 *
 * Store a bounded COPY of the iov ARRAY into the slot (pointers + lengths). Small segments whose
 * data pointer may be transient (the driver's stack Content-Length scratch) are snapshotted into
 * the slot's preallocated head buffer; larger segments (body / large header block, all owned by
 * the live KlResponse) are referenced in PLACE. This copies at most KL_LWR_TX_HEAD bytes of tiny
 * head data — NEVER the payload. Returns 0 on installed, -1 on an iov-count / snapshot-overflow
 * limit (the caller closes the conn). */
static int lwr_store_iov(KlLwrCtx *ctx, KlLwrConn *cs, const KlLwrIoVec *iov, int iovcnt,
                         size_t *total_out) {
    if (iovcnt < 0 || iovcnt > KL_LWR_MAX_TX_IOV) return -1;   /* documented bounded limit */
    unsigned char *head = lwr_slot_tx_head(ctx, cs);
    size_t hu = 0, total = 0;
    for (int i = 0; i < iovcnt; i++) {
        size_t len = iov[i].len;
        if (len > SIZE_MAX / 2 || total > SIZE_MAX / 2) return -1;   /* overflow guard */
        if (len == 0) { cs->iov[i].base = (const unsigned char *)""; cs->iov[i].len = 0; continue; }
        if (len <= KL_LWR_TX_SNAP) {
            /* Snapshot small (possibly transient) segments into the preallocated head buffer. */
            if (hu > KL_LWR_TX_HEAD - len) return -1;   /* head buffer full (bounded) */
            memcpy(head + hu, iov[i].base, len);
            cs->iov[i].base = head + hu;
            hu += len;
        } else {
            cs->iov[i].base = (const unsigned char *)iov[i].base;   /* reference in place */
        }
        cs->iov[i].len = len;
        total += len;
    }
    cs->iovcnt = iovcnt;
    cs->tx_head_used = hu;
    *total_out = total;
    return 0;
}

/* Common install tail: mark the send active + pump, or synthesize an immediate empty completion. */
static void lwr_send_start(struct tcp_pcb *p, KlLwrConn *cs) {
    if (cs->send_total == 0) {       /* nothing to send — synthesize an immediate completion */
        cs->pend_write = 1;
        cs->pend_write_bytes = 0;
        lwr_send_reset(cs);
        return;
    }
    cs->send_active = 1;
    lwr_send_pump(p, cs);
}

int kl_lwr_send_begin(void *lwrctx, void *pcb, const KlLwrIoVec *iov, int iovcnt) {
    KlLwrCtx *ctx = lwrctx;
    struct tcp_pcb *p = (struct tcp_pcb *)pcb;
    KlLwrConn *cs = lwr_conn_find(ctx, p);
    if (!cs) return -1;
    lwr_send_reset(cs);              /* one send in flight per conn (completion contract) */
    size_t total = 0;
    if (lwr_store_iov(ctx, cs, iov, iovcnt, &total) != 0) { lwr_send_reset(cs); return -1; }
    cs->is_file    = 0;
    cs->send_total = total;
    lwr_send_start(p, cs);
    return 0;
}

int kl_lwr_sendfile_begin(void *lwrctx, void *pcb, const KlLwrIoVec *head, int head_n,
                          int file_fd, uint64_t count) {
    KlLwrCtx *ctx = lwrctx;
    struct tcp_pcb *p = (struct tcp_pcb *)pcb;
    KlLwrConn *cs = lwr_conn_find(ctx, p);
    if (!cs) return -1;
    lwr_send_reset(cs);
    size_t head_total = 0;
    if (lwr_store_iov(ctx, cs, head, head_n, &head_total) != 0) { lwr_send_reset(cs); return -1; }
    if (count > (uint64_t)(SIZE_MAX - head_total)) { lwr_send_reset(cs); return -1; }  /* overflow */
    cs->is_file     = 1;
    cs->file_fd     = file_fd;       /* borrowed — the response layer owns closing it (no dup) */
    cs->file_count  = count;
    cs->head_total  = head_total;
    cs->send_total  = head_total + (size_t)count;
    lwr_send_start(p, cs);
    return 0;
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
