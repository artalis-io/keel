/*
 * lwip_raw_glue.c: the lwIP-touching half of the raw completion backend.
 *
 * This TU includes ONLY lwIP's NO_SYS=1 raw headers, never KEEL's socket/net headers,
 * so lwIP's htons/ntohs macros and its ssize_t typedef cannot clash with the host
 * <sys/socket.h>/<netinet/in.h> that event_lwip_raw.c pulls in through the KEEL socket
 * seam. The two halves meet across lwip_raw_glue.h (opaque void* pcb/netif/lwrctx + neutral
 * KlLwrRecord; no lwIP type escapes).
 *
 * ── Receive data model ──────────────────────────────────────────────
 * A per-context/per-connection data model with four coupled correctness properties:
 *
 *   #1 no receive truncation / ack-before-deliver. Each slot owns a bounded RETAINED
 *      pbuf chain (real lwIP flow control): the recv callback either RETAINS the pbuf (no
 *      copy, no ack) or, at the per-conn bound, returns ERR_MEM WITHOUT freeing (lwIP holds it
 *      and re-delivers = backpressure). tcp_recved() is issued ONLY as bytes are copied out
 *      into Keel's read_buf during the drain. No byte is ever acked before it is delivered,
 *      and none is silently dropped.
 *
 *   #2 arm-capacity match. Arm state lives in the per-conn slot (`armed`), and the slot
 *      table is sized to conn_cap = KlHttpServerConfig.max_connections: ONE authoritative
 *      limit. A conn that has a slot always arms.
 *
 *   #4 no silent completion drop. Each slot carries its own pending-completion flags
 *      (accept / write); the drain SCANS all slots and emits one completion per pending item,
 *      bounded by conn_cap. It cannot overflow, and a failed completion is a per-slot flag ->
 *      always deliverable. Every posted op completes exactly once, also on a dead conn.
 *
 *   #5 no global mutable state. State lives in an opaque per-context KlLwrCtx (allocated
 *      through KlAllocator at ctx create, not in a callback or hot path). The lwIP CORE
 *      (lwip_init) stays a process-global one-time init; a single file-scope "active ctx"
 *      guard enforces the NO_SYS=1 single-stack invariant (rejecting a 2nd simultaneous ctx)
 *      and is the ONLY legitimate global; sequential create/destroy/create works.
 *
 * ── Send + file-response data model ──────────────────────────────────────────────
 * The send + file-response path uses BOUNDED, PREALLOCATED per-connection
 * transmit state, no per-response malloc, no double-copy of the whole payload, no whole-file
 * read into memory:
 *
 *   - A fixed KL_LWR_TX_WIN (32 KiB) staging buffer is kl_malloc'd ONCE PER SLOT at ctx create
 *     (a single conn_cap * KL_LWR_TX_WIN block, sliced per slot). The send path performs ZERO
 *     allocation: no malloc/free/calloc/realloc remains in this TU.
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
 *     response size, so response size is unbounded-by-design (no fixed response-size cap).
 *
 *   - File response (kl_lwr_sendfile_begin): does NOT read the file into memory. The slot stores
 *     the head iov (bounded, snapshotted as above) + file_fd + count + sent_off. The pump sends
 *     the head first, then preads the next <= min(tcp_sndbuf, KL_LWR_TX_WIN) chunk from file_fd
 *     into the TX window and tcp_write(COPY), advancing the file offset; refills on each tcp_sent.
 *     Short reads loop; a pread error or EOF aborts the conn with a FAILED WRITE (ok=0) so the driver
 *     closes. The glue only READS file_fd; the response layer owns closing it (no double-close).
 *
 * SPDX-License-Identifier: MIT
 */
#include "lwip_raw_glue.h"

#include "lwip/init.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"     /* datagram pcb (udp_new/bind/recv/sendto/remove) */
#include "lwip/timeouts.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/sys.h"
#include "lwip/pbuf.h"

#include <keel/allocator.h>   /* KlAllocator + kl_malloc/kl_free: the per-context state.
                               * Only KlAllocator (a plain vtable of fn ptrs + sizes) crosses;
                               * it pulls NO socket/net type, so the lwIP-header seam holds. */

#include <string.h>
#include <time.h>
#include <stdint.h>   /* SIZE_MAX (sendfile size overflow guard) */
#include <unistd.h>   /* pread + off_t for the file-send path */

/* NOTE: NO <stdlib.h>: the send path uses no malloc/free. All glue-owned
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
 * buffer is a separate transmit allocation). Overridable at build time
 * (-DKL_LWR_RX_MAX=...) so a small-config test can shrink it below TCP_WND to deterministically
 * exercise the ERR_MEM backpressure path. */
#ifndef KL_LWR_RX_MAX
#define KL_LWR_RX_MAX (64u * 1024u)
#endif

/* tcp_write's u16_t len ceiling (send-pump chunking). */
#define KL_LWR_TCP_WRITE_MAX 0xffffu

/* ── UDP datagram-completion path (datagram over raw) ───────────────────
 * A datagram slot mirrors the TCP conn slot but for a udp_pcb. lwIP's udp_recv callback holds ONE
 * inbound datagram (the ONE-HELD-PACKET contract, docs/archive/designs/datagram_step7_public_api_design.md §3):
 * the payload is copied out of the pbuf + source IPv4/port into the slot's single held datagram,
 * which the drain surfaces as KL_COMP_DGRAM_RECV (gated by `rx_armed`, one in-flight recv). A
 * second datagram arriving while one is already held is DROPPED (deterministic UDP loss, a
 * completion backend with no socket receive buffer holds exactly the one datagram the posted recv
 * op will take). Sends go straight out (udp_sendto) and the drain surfaces the matching
 * KL_COMP_DGRAM_SEND. See the design notes in event_lwip_raw.c and docs.
 *
 * The single held slot matches the KlDgramRecv one-in-flight/one-held contract exactly (no
 * over-buffering that could deliver stale datagrams a paused/slow consumer never posted ops for). */

/* Depth of the pending-SEND completion FIFO per udp slot (a burst of udp_sendto between drains).
 * Distinct from receive, which is now a single held datagram (one-held-packet contract). */
#ifndef KL_LWR_UDP_SEND_RING
#define KL_LWR_UDP_SEND_RING 16
#endif

/* Largest single datagram payload retained (a datagram larger than this is truncated on capture,
 * with the truncated flag set, mirrors MSG_TRUNC). 2 KiB comfortably spans a loopback datagram;
 * bounded so the ring is a fixed preallocated block, not per-datagram malloc. */
#ifndef KL_LWR_UDP_DGRAM_MAX
#define KL_LWR_UDP_DGRAM_MAX 2048u
#endif

/* Number of udp slots per ctx. Small: loopback tests use one or two datagram sockets, and
 * MEMP_NUM_UDP_PCB (lwipopts_raw.h) caps concurrent udp pcbs anyway. */
#ifndef KL_LWR_UDP_SLOTS
#define KL_LWR_UDP_SLOTS 8
#endif

/* ── Bounded, preallocated per-connection transmit window ────────────────
 * KL_LWR_TX_WIN is the fixed staging buffer each slot pumps THROUGH, allocated ONCE at ctx
 * create (a single conn_cap * KL_LWR_TX_WIN block, sliced per slot). The pump copies at most
 * KL_LWR_TX_WIN bytes per round out of the referenced iov segments (buffered) or pread out of
 * the file (file), tcp_write(COPY)s them, and advances. Because transmit memory is bounded by
 * this window, NOT by the response/file size, a response of ANY size streams in constant
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
 * limit). Kept in sync with the driver's kl_http_response_build_iovec cap (7). */
#define KL_LWR_MAX_TX_IOV 8

/* A per-slot preallocated head-snapshot buffer holds a COPY of the small iov segments whose data
 * pointer may be transient (the driver's stack Content-Length scratch `cl_buf`); any segment at
 * or below KL_LWR_TX_SNAP bytes is snapshotted here at send_begin; larger segments (body / large
 * header block, all owned by the live KlHttpResponse) are referenced in place. Sized to hold every
 * snapshotted segment of one send: KL_LWR_MAX_TX_IOV * KL_LWR_TX_SNAP. */
#define KL_LWR_TX_SNAP 256u
#define KL_LWR_TX_HEAD (KL_LWR_MAX_TX_IOV * KL_LWR_TX_SNAP)

/* ── per-connection slot ─────────────────────────────────────────────────────────
 * One slot per accepted connection; the slot array is sized to conn_cap (== max_connections).
 * A free slot has pcb == NULL and !dead. A `dead` slot's pcb was freed (->pcb is NULL); it stays
 * reserved until the backend closes its handle, so its outstanding ops can complete against it.
 *
 * An ACCEPTED connection is known to the backend by a slot handle (index + generation, see
 * lwr_handle_of), never by its pcb pointer: lwIP's memp pools are LIFO, so a freed pcb's address
 * is the next accept's pcb, and a pointer-keyed close or cancel of a dead connection would reach
 * the new one. The generation is bumped each time the slot is taken, so a stale handle matches
 * nothing. CLIENT slots (outbound connects) are still keyed by their pcb pointer. */
typedef struct {
    struct tcp_pcb *pcb;        /* NULL = free or dead slot */
    void           *owner;      /* KlHttpConn* the backend associated (tcp_arg) */
    uintptr_t       gen;        /* bumped on every reuse of the slot (stale-handle guard) */

    /* ── retained receive queue (fix #1) ──────────────────────────────────────────
     * rx_head is a retained pbuf chain (oldest first, appended with pbuf_cat). We OWN it and
     * dequeue bytes from its front as we deliver them (pbuf_free_header). rx_queued = total
     * retained (un-delivered) bytes: the KL_LWR_RX_MAX bound + the has-data status. */
    struct pbuf    *rx_head;
    size_t          rx_queued;

    int             armed;      /* a recv is posted (single in-flight recv per conn): it completes
                                 * with data, or with a failed READ once the peer closed / the
                                 * conn died, whatever else happens to the conn */
    void           *recv_buf;   /* armed recv: caller-chosen destination buffer (raw bytes) */
    size_t          recv_cap;   /* armed recv: destination capacity */
    int             closed;     /* peer closed / errored: an armed recv fails after rx drains */
    int             dead;       /* pcb freed (tcp_err / abort): ->pcb is NULL */
    void           *dead_fd;    /* CLIENT slots: the (freed) pcb pointer the client still holds as
                                 * its fd, for its close / EOF correlation only. NOT used to reach
                                 * lwIP. ->pcb is NULL, so lwr_conn_find never aliases it. */

    /* ── per-slot pending completions (fix #4, replaces the global ring) ──────────
     * Each is at most 1 pending; the drain scans slots and emits one completion per set flag,
     * clearing it. Bounded by conn_cap -> cannot overflow. Every posted op gets exactly one:
     * a posted send its WRITE (ok=1 once acked, ok=0 if the conn dies first), an armed recv its
     * READ (see armed). */
    int             pend_accept;      /* ACCEPT waiting to be surfaced */
    uint8_t         peer_ip[4];       /* ACCEPT peer IPv4 (network order) */
    uint16_t        peer_port;        /* ACCEPT peer port (host order) */
    int             send_posted;      /* a send was posted and its WRITE not yet surfaced */
    int             pend_write;       /* the posted send's WRITE is waiting to be surfaced */
    int             pend_write_ok;    /* ...as a success (1) or a failure (0) */
    size_t          pend_write_bytes; /* bytes acked for the pending WRITE */

    /* ── outbound client state ─────────────────────────────────────────────
     * A client slot is created by kl_lwr_connect for a client pcb (tcp_connect). It reuses the
     * SAME retained-recv queue (rx_head/rx_queued) for the RESPONSE (lwr_srv_recv is direction-
     * agnostic). But its completions do NOT flow through the server-side KL_COMP_ACCEPT/READ/WRITE/
     * terminal path (those need a KlHttpConn target the driver owns). Instead:
     *   - the connect result surfaces as KL_LWR_CONNECT (pend_connect + connect_ok), routed to the
     *     client's tagged watcher as KL_COMP_CONNECT;
     *   - the data plane rides the client's readiness watcher: the backend records the armed watcher
     *     (watcher_udata + watcher_mask, captured at lwr_ev_add/mod) and the drain relays it as
     *     KL_COMP_WATCHER when the pcb is writable (sndbuf headroom) or readable (rx queued/closed).
     *     The client's kl_sock_send/kl_sock_recv on the pcb are real (kl_lwr_client_send/_recv).
     * No KlHttpConn, no server-side completion path; the seam holds. The request send does NOT use the
     * send-pump (that surfaces a server KL_COMP_WRITE); the client writes via kl_lwr_client_send. */
    int             is_client;        /* 1 = an outbound client pcb (kl_lwr_connect) */
    int             connected;        /* client: TCP connected (connected_cb fired ERR_OK) */
    int             pend_connect;     /* client: a connect completion is waiting to be surfaced */
    int             connect_ok;       /* client: 1 = connected, 0 = failed (with pend_connect) */
    void           *watcher_udata;    /* client: the tagged KlWatcher udata (connect + data plane) */
    unsigned        watcher_mask;     /* client: armed readiness mask (KL_LWR_EV_READ/WRITE) */

    /* ── outgoing send state (BOUNDED, PREALLOCATED, no whole-payload copy) ──
     * A send references its source in place (the live KlHttpResponse segments) and pumps THROUGH a
     * fixed preallocated window. `send_active` = a send is in flight. `send_total` = the whole
     * logical payload; `send_off` = bytes
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
     * send. The pump window + head-snapshot buffers themselves are NOT cached here; they are
     * derived from the slot index into the ctx's single preallocated TX block (lwr_slot_tx_win /
     * lwr_slot_tx_head), so they survive the memset in lwr_slot_clear / lwr_conn_alloc without a
     * re-bind step. */
    size_t          tx_head_used;
} KlLwrConn;

/* ── The single held inbound datagram in a udp slot ──────────────────
 * The payload is copied out of the lwIP pbuf at capture (so the pbuf is freed immediately, no
 * pbuf retained past the callback), bounded to KL_LWR_UDP_DGRAM_MAX. src_ip/src_port carry the
 * datagram source (IPv4 network-order bytes + host-order port). One per slot (one-held-packet). */
typedef struct {
    unsigned char data[KL_LWR_UDP_DGRAM_MAX];
    size_t        len;
    int           truncated;      /* 1 = payload was larger than the buffer (MSG_TRUNC-like) */
    uint8_t       src_ip[4];      /* source IPv4, network order */
    uint16_t      src_port;       /* source port, host order */
} KlLwrDgram;

/* Stable-liveness token for datagram completion ops (src/completion_life.h, the frozen "backend-owned
 * stable token", docs/contracts/datagram.md §6). Forward-declared locally so this lwIP-only glue TU
 * keeps its minimal include set (no src/ header path); the two entrypoints resolve against libkeel's
 * completion_life.o at link. Each posted udp op (recv arm / pending send) holds ONE ref; the drain
 * transfers it to the completion event, and close/teardown release any op ref not yet drained. */
typedef struct KlCompLife KlCompLife;
void kl_comp_life_retain(KlCompLife *l);
void kl_comp_life_release(KlCompLife *l);

/* ── per-udp-socket slot ───────────────────────────────────────────────────
 * One slot per datagram bound over the raw backend. `pcb == NULL` = free slot. `life` is the stable
 * token the op retained at post (the drain transfers it to the KL_COMP_DGRAM_RECV/SEND event, so the
 * backend never dereferences the possibly-freed transport owner). The slot holds exactly
 * `(rx_armed ? 1 : 0) + pend_send` token refs; close/teardown release that many. The receive side is
 * ONE held datagram (`held`/`has_held`, the one-held-packet contract); a second arrival while one is
 * held is dropped, never buffered. `rx_armed` gates delivery to the completion contract's one-in-flight
 * recv. `pend_send` counts sends whose KL_COMP_DGRAM_SEND the drain still owes. */
typedef struct {
    struct udp_pcb *pcb;          /* NULL = free slot */
    KlCompLife    *life;         /* stable token; one ref per outstanding op (arm + pending sends) */
    KlLwrDgram      held;         /* the ONE held inbound datagram (one-held-packet contract) */
    int             has_held;     /* 1 = `held` carries an undelivered datagram */
    int             rx_armed;     /* a recv is posted (surface the held datagram on drain) */
    int             pend_send;    /* pending KL_COMP_DGRAM_SEND completions owed */
    KlLwrDgram      staged;       /* the datagram currently surfaced (buf stays valid this drain) */

    /* Bounded FIFO of pending completed-send byte counts (each surfaces one KL_COMP_DGRAM_SEND).
     * A datagram send completes synchronously (udp_sendto), so this is drained promptly; the ring
     * is sized generously enough that a burst of sends between drains is not lost. On overflow the
     * oldest pending send len is coalesced (never lost as an accounting quantity, see enqueue). */
    size_t          send_len[KL_LWR_UDP_SEND_RING];
    int             send_head;
} KlLwrUdpSlot;

/* ── per-context backend state (fix #5, de-globalize) ────────────────────────────
 * All per-backend raw state lives here, allocated through KlAllocator at ctx create. */
typedef struct KlLwrCtx {
    KlAllocator    *alloc;
    struct tcp_pcb *listen_pcb;   /* the relocated LISTEN pcb (tcp_listen result), or NULL */
    struct netif   *loopif;       /* the loopback netif */
    KlLwrConn      *conns;        /* conn_cap slots (kl_malloc'd) */
    int             conn_cap;
    KlLwrUdpSlot    udp[KL_LWR_UDP_SLOTS];   /* udp slots (fixed, in-ctx) */
    /* Context-owned pending RECV terminal completions (a cancelled armed recv). Preallocated (one
     * per udp slot); SURVIVES the slot teardown so the terminal drains even after the pcb closes. Each
     * non-NULL entry holds ONE transferred arm token ref; the drain emits one terminal + releases it. */
    KlCompLife    *udp_term[KL_LWR_UDP_SLOTS];   /* NULL = free */

    /* ── preallocated transmit memory (kl_malloc'd once, sliced per slot) ──
     * ONE contiguous block of conn_cap * KL_LWR_TX_STRIDE bytes; slot i owns bytes
     * [i*STRIDE, i*STRIDE + KL_LWR_TX_WIN) as its pump window and the following KL_LWR_TX_HEAD
     * bytes as its head-snapshot buffer. Never grown per-send; transmit memory is bounded.
     * tx_block_size is kept for the exact kl_free at destroy. */
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
 * callbacks fire inline on that ctx's tick, NO_SYS=1 single-thread, no locks). */
static int       g_lwip_inited = 0;
static KlLwrCtx *g_active_ctx = NULL;

static KlLwrCtx *lwr_ctx(void) { return g_active_ctx; }

void *kl_lwr_active_ctx(void) { return g_active_ctx; }

/* Find a slot by its LIVE pcb pointer. A dead slot has ->pcb == NULL, so it NEVER matches; this
 * is what makes the raw callbacks (recv/sent/err resolve the owning slot from tpcb) safe against
 * lwIP reusing a freed pcb address for a NEW accept: the new conn's callbacks resolve to the new
 * slot, never to the corpse of the conn that previously held that address. */
static KlLwrConn *lwr_conn_find(KlLwrCtx *ctx, const struct tcp_pcb *pcb) {
    if (!ctx || pcb == NULL) return NULL;
    for (int i = 0; i < ctx->conn_cap; i++)
        if (ctx->conns[i].pcb == pcb) return &ctx->conns[i];
    return NULL;
}

/* Find a CLIENT slot by the client's fd (its pcb pointer): a LIVE client slot whose ->pcb
 * matches, OR a DEAD one whose ->dead_fd matches (the freed pointer the client still holds).
 * Server connections never match: they are addressed by slot handle (lwr_srv_slot), so a pointer
 * that an accept reused can never resolve to one. A live match is preferred. */
static KlLwrConn *lwr_client_by_fd(KlLwrCtx *ctx, const void *fd) {
    if (!ctx || fd == NULL) return NULL;
    KlLwrConn *dead = NULL;
    for (int i = 0; i < ctx->conn_cap; i++) {
        KlLwrConn *c = &ctx->conns[i];
        if (!c->is_client) continue;
        if (c->pcb == (const struct tcp_pcb *)fd) return c;        /* live */
        if (c->dead && c->dead_fd == fd) dead = c;
    }
    return dead;
}

/* ── server connection handles ────────────────────────────────────────────────────
 * The KlSocketHandle of an accepted connection, crossing the seam as an opaque void *:
 *   bit 0       1 (a pcb pointer is always aligned, so an odd value is never one)
 *   bits 1..16  slot index (conn_cap <= KL_LWR_MAX_CONNS keeps it below 0xffff, so the handle is
 *               never all ones, i.e. never KL_INVALID_SOCKET, even where intptr_t is 32 bits)
 *   bits 17..30 slot generation (low 14 bits; wraps only after 16384 reuses of one slot)
 *   bit 31      1, so a handle that strays into a host socket call as an int is negative (EBADF),
 *               never a real descriptor
 * Nothing in the handle is dereferenced; a stale one matches no slot. */
#define KL_LWR_H_TAG        ((uintptr_t)0x80000001u)
#define KL_LWR_H_IDX_SHIFT  1
#define KL_LWR_H_IDX_MASK   ((uintptr_t)0xffffu)
#define KL_LWR_H_GEN_SHIFT  17
#define KL_LWR_H_GEN_MASK   ((uintptr_t)0x3fffu)
#define KL_LWR_MAX_CONNS    0xffff

/* A slot handle carries both tag bits (and nothing above bit 31): a pcb pointer is never odd, and
 * a stray odd value without the high tag bit is not taken for a slot. */
static int lwr_is_handle(const void *h) {
    uintptr_t v = (uintptr_t)h;
    return (v & KL_LWR_H_TAG) == KL_LWR_H_TAG && (v >> 31) == 1u;
}

static void *lwr_handle_of(KlLwrCtx *ctx, KlLwrConn *c) {
    uintptr_t idx = (uintptr_t)(c - ctx->conns);
    return (void *)(KL_LWR_H_TAG | ((c->gen & KL_LWR_H_GEN_MASK) << KL_LWR_H_GEN_SHIFT) |
                    (idx << KL_LWR_H_IDX_SHIFT));
}

/* The SERVER slot a handle names: in range, taken (live or dead), not a client, and of the
 * handle's generation. NULL for anything else (a pcb pointer, a stale or foreign handle). */
static KlLwrConn *lwr_srv_slot(KlLwrCtx *ctx, const void *h) {
    if (!ctx || !lwr_is_handle(h)) return NULL;
    uintptr_t v = (uintptr_t)h;
    uintptr_t idx = (v >> KL_LWR_H_IDX_SHIFT) & KL_LWR_H_IDX_MASK;
    if (idx >= (uintptr_t)ctx->conn_cap) return NULL;
    KlLwrConn *c = &ctx->conns[idx];
    if ((c->pcb == NULL && !c->dead) || c->is_client) return NULL;
    if ((c->gen & KL_LWR_H_GEN_MASK) != ((v >> KL_LWR_H_GEN_SHIFT) & KL_LWR_H_GEN_MASK)) return NULL;
    return c;
}

/* The LIVE server slot a handle names (its pcb is still lwIP's), else NULL. */
static KlLwrConn *lwr_srv_live(KlLwrCtx *ctx, const void *h) {
    KlLwrConn *c = lwr_srv_slot(ctx, h);
    return (c && !c->dead && c->pcb) ? c : NULL;
}

/* Free the whole retained rx pbuf chain (cancellation / close / destroy, no leak). */
static void lwr_rx_free(KlLwrConn *c) {
    if (c->rx_head) { pbuf_free(c->rx_head); c->rx_head = NULL; }
    c->rx_queued = 0;
}

/* Reset the slot's send offsets to "no send in flight". Nothing to free; the TX
 * window + head-snapshot buffers are preallocated (ctx-owned, index-derived) and reused across
 * sends. A reset slot's send_active is 0; file_fd is set to -1 defensively so no stale fd is
 * ever pread. This is the only send teardown; there is no heap buffer to release. */
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
 * (the accept path then tcp_abort's the new pcb, never represented). */
static KlLwrConn *lwr_conn_alloc(KlLwrCtx *ctx, struct tcp_pcb *pcb) {
    for (int i = 0; i < ctx->conn_cap; i++)
        if (ctx->conns[i].pcb == NULL && !ctx->conns[i].dead) {
            uintptr_t gen = ctx->conns[i].gen + 1;   /* every handle to an earlier occupant is stale */
            memset(&ctx->conns[i], 0, sizeof(ctx->conns[i]));
            lwr_send_reset(&ctx->conns[i]);   /* normalize send state (file_fd=-1); TX buffers are
                                               * index-derived so the memset did not lose them */
            ctx->conns[i].gen = gen;
            ctx->conns[i].pcb = pcb;
            return &ctx->conns[i];
        }
    return NULL;
}

/* Fully release a slot back to free (pcb==NULL) after releasing owned resources (the rx pbuf
 * chain; the send state has no heap; the TX window is preallocated). Recycling: a freed slot is
 * immediately reusable by the next accept, and its NULL pcb can never alias a new pcb's pointer.
 * The TX window/head buffers are index-derived (not stored in the slot), so the memset does not
 * lose them. */
static void lwr_slot_clear(KlLwrConn *c) {
    lwr_rx_free(c);
    uintptr_t gen = c->gen;     /* kept: the next lwr_conn_alloc bumps it */
    memset(c, 0, sizeof(*c));   /* pcb=NULL + all flags/pending cleared (send offsets zeroed) */
    c->gen = gen;
}

/* The posted send can no longer finish (the conn died, or the peer closed under it): drop it and
 * owe its WRITE as a failure. A no-op without a posted send, or when its WRITE is already owed. */
static void lwr_fail_send(KlLwrConn *c) {
    if (!c->send_posted || c->pend_write) return;
    lwr_send_reset(c);
    c->pend_write = 1;
    c->pend_write_ok = 0;
    c->pend_write_bytes = 0;
}

/* The pcb is gone (lwIP freed it, or is about to): release the slot's rx chain, fail the posted
 * send, and mark the slot dead. The slot stays reserved for the backend's close; an armed recv
 * completes as a failed READ through kl_lwr_next_readable. Never touches the pcb. */
static void lwr_mark_dead(KlLwrConn *c) {
    lwr_rx_free(c);
    lwr_fail_send(c);
    lwr_send_reset(c);
    c->dead_fd = c->pcb;
    c->pcb = NULL;
    c->dead = 1;
    c->closed = 1;
}

/* Abort a LIVE server connection: mark the slot dead first, detach the callbacks (so lwIP's err
 * callback does not re-enter it), then tcp_abort, which frees the pcb and sends an RST. Inside
 * an lwIP callback for this pcb the caller must then return ERR_ABRT. */
static void lwr_srv_kill(KlLwrConn *c) {
    struct tcp_pcb *p = c->pcb;
    lwr_mark_dead(c);
    tcp_arg(p, NULL);
    tcp_recv(p, NULL);
    tcp_sent(p, NULL);
    tcp_err(p, NULL);
    tcp_abort(p);
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
    if (!alloc || conn_cap <= 0 || conn_cap > KL_LWR_MAX_CONNS) return NULL;   /* handle index bound */
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

    /* Preallocate the bounded per-conn transmit block (zero send-path alloc). A
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
    if (conn_cap > KL_LWR_MAX_CONNS) return -1;   /* the server handle carries a 16-bit index */
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
    /* Grow before any accept: no live slots to relocate; a fresh array is simplest + correct. */
    KlLwrConn *nc = kl_realloc(ctx->alloc, ctx->conns, obytes, nbytes);
    if (!nc) return -1;   /* TX block already grown: harmless (larger than needed); ctx usable */
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
        if (!dead) {                 /* live pcb: detach callbacks then abort (frees it) */
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

    /* Tear down any live udp pcbs (detach the recv cb + free the pcb). The single held
     * datagram is inline in the slot (no heap), cleared by the memset below implicitly. A slot
     * still live here means the datagram close / kl_lwr_udp_close never ran (loop torn down before the
     * socket): release its outstanding op token refs so they don't leak (the owner ref may still
     * leak, the same free-sockets-before-the-loop contract as every backend). */
    for (int i = 0; i < KL_LWR_UDP_SLOTS; i++) {
        KlLwrUdpSlot *s = &ctx->udp[i];
        if (s->pcb) {
            for (int k = (s->rx_armed ? 1 : 0) + s->pend_send; k > 0; k--)
                kl_comp_life_release(s->life);
            udp_recv(s->pcb, NULL, NULL);
            udp_remove(s->pcb);
            s->pcb = NULL;
        }
    }
    /* Release any pending recv terminal never drained (loop torn down before the drain); each
     * holds one transferred arm ref. Symmetric with the arm/send release above. */
    for (int t = 0; t < KL_LWR_UDP_SLOTS; t++)
        if (ctx->udp_term[t]) { kl_comp_life_release(ctx->udp_term[t]); ctx->udp_term[t] = NULL; }

    size_t bytes = (size_t)ctx->conn_cap * sizeof(KlLwrConn);
    KlAllocator *alloc = ctx->alloc;
    kl_free(alloc, ctx->conns, bytes);
    kl_free(alloc, ctx->tx_block, ctx->tx_block_size);   /* the preallocated TX block */
    if (g_active_ctx == ctx) g_active_ctx = NULL;   /* clear the guard: sequential create OK */
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
 *     drop it and fail its WRITE now; an armed recv fails once its retained bytes are out.
 *   - at the per-conn bound (rx_queued + p->tot_len > KL_LWR_RX_MAX): return ERR_MEM WITHOUT
 *     freeing/queuing/acking p; lwIP retains p and re-delivers later (real backpressure).
 *   - else RETAIN p: append to rx_head (pbuf_cat, refcount-aware, NO copy, NO pbuf_free), add
 *     to rx_queued, return ERR_OK. We now OWN the chain and free it as we consume it.
 * tcp_recved() is NEVER called here; only as bytes are copied out in kl_lwr_take_staged. */
static err_t lwr_srv_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)arg;
    KlLwrCtx *ctx = lwr_ctx();
    KlLwrConn *cs = lwr_conn_find(ctx, tpcb);

    if (err != ERR_OK || p == NULL) {
        if (p) pbuf_free(p);
        if (cs) {
            cs->closed = 1;              /* an armed recv fails once rx drains (next_readable) */
            if (cs->send_active)         /* close-with-outstanding: tear down promptly */
                lwr_fail_send(cs);       /* drop the in-flight send; its WRITE fails */
        }
        return ERR_OK;   /* keep the pcb; the driver's close tears it down */
    }

    if (!cs) {                            /* no slot for this pcb: cannot receive; drop cleanly.
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

/* ── send-pump: fill the preallocated TX window from the source, then tcp_write ──────
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
            if (nr < 0) return -1;            /* file read error: caller surfaces ok=0 terminal */
            if (nr == 0) break;               /* unexpected EOF: send what we gathered */
            got += (size_t)nr;                /* short read: loop and pread again */
        }
    }
    return (ssize_t)got;
}

/* Pump: hand as many bytes as tcp_sndbuf + KL_LWR_TX_WIN allow into tcp_write, in TX-window-sized
 * rounds, advancing send_off. Stops on ERR_MEM (backpressure, resumed by tcp_sent) or when the
 * whole payload is written. A file read error or a hard tcp_write error aborts the pcb (the
 * posted send's WRITE fails, an armed recv fails too) and the driver closes.
 * Returns 1 if it aborted the pcb (freed it), else 0. Called from the tcp_sent callback, a 1 MUST
 * become ERR_ABRT: lwIP's tcp_input keeps using the pcb after any other return. */
static int lwr_send_pump(struct tcp_pcb *pcb, KlLwrConn *cs) {
    KlLwrCtx *ctx = lwr_ctx();
    if (!cs || !cs->send_active || !ctx) return 0;
    unsigned char *win = lwr_slot_tx_win(ctx, cs);
    int wrote_any = 0;
    while (cs->send_off < cs->send_total) {
        u16_t sndbuf = tcp_sndbuf(pcb);
        if (sndbuf == 0) break;                       /* no headroom: wait for tcp_sent */
        size_t remain = cs->send_total - cs->send_off;
        size_t chunk = remain;
        if (chunk > sndbuf) chunk = sndbuf;
        if (chunk > KL_LWR_TX_WIN) chunk = KL_LWR_TX_WIN;
        if (chunk > KL_LWR_TCP_WRITE_MAX) chunk = KL_LWR_TCP_WRITE_MAX;

        ssize_t got = lwr_gather(cs, cs->send_off, win, chunk);
        /* got < 0 = file read error; got == 0 while bytes remain = truncated file (declared more
         * than the file yields). Either is a mid-transmission failure the client cannot recover
         * from (a short body under a fixed Content-Length): abort the conn, failing the WRITE
         * (ok=0) so the driver closes. We NEVER silently under-deliver a declared-length body. */
        if (got <= 0) {
            lwr_srv_kill(cs);                         /* WRITE fails (ok=0); frees the pcb */
            return 1;
        }

        err_t w = tcp_write(pcb, win, (u16_t)got, TCP_WRITE_FLAG_COPY);
        if (w == ERR_MEM) break;                      /* queue full: backpressure, resume later */
        if (w != ERR_OK) { lwr_srv_kill(cs); return 1; }   /* hard error: abort the conn */
        cs->send_off += (size_t)got;
        wrote_any = 1;
    }
    if (wrote_any) tcp_output(pcb);                   /* flush the freshly-queued segments */
    return 0;
}

/* sent callback: `len` bytes acked. Advance acked, pump more, and only when the WHOLE payload is
 * acknowledged mark the send's single WRITE completion pending + reset the send offsets (no
 * buffer to free; the TX window is preallocated + reused). */
static err_t lwr_srv_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    (void)arg;
    KlLwrCtx *ctx = lwr_ctx();
    KlLwrConn *cs = lwr_conn_find(ctx, tpcb);
    if (!cs || !cs->send_active) return ERR_OK;       /* stray ack (no send in flight) */
    cs->send_acked += len;
    if (cs->send_acked >= cs->send_total) {
        cs->pend_write = 1;
        cs->pend_write_ok = 1;
        cs->pend_write_bytes = cs->send_total;
        lwr_send_reset(cs);                           /* send complete: clear offsets */
        return ERR_OK;
    }
    /* Push more of the tail. If the pump aborted the pcb it is freed: only ERR_ABRT stops lwIP's
     * tcp_input from using it after this callback returns. */
    if (lwr_send_pump(tpcb, cs)) return ERR_ABRT;
    return ERR_OK;
}

/* err callback: the pcb was aborted by the stack (RST/OOM/self-initiated). lwIP has ALREADY
 * FREED the pcb; we MUST NOT dereference it. `arg` is the accepted SLOT (tcp_arg set in
 * lwr_srv_accept, and NOT overwritten by kl_lwr_set_owner), so we resolve the slot directly;
 * even in the accept->post_recv window before the driver adopts the conn. Keying on the slot
 * (not the owner, which is still NULL in that window) ensures an err there is never dropped,
 * which would otherwise leave a dangling pcb + pend_accept the driver would then adopt (a
 * use-after-free). We free the rx chain, fail the posted send, mark the slot dead + closed,
 * then either recycle it (aborted before adoption, no KlHttpConn exists to notify) or leave it
 * for the driver's close, each posted op completing as a failure meanwhile. */
static void lwr_srv_err(void *arg, err_t err) {
    (void)err;
    KlLwrCtx *ctx = lwr_ctx();
    if (!ctx || !arg) return;
    KlLwrConn *c = (KlLwrConn *)arg;
    /* Defensive: arg must be one of this ctx's slots (never a client watcher, clients use
     * lwr_cli_err). Reject anything out of range or already torn down (idempotent on a double err). */
    if (c < ctx->conns || c >= ctx->conns + ctx->conn_cap) return;
    if (c->dead || c->pcb == NULL) return;

    /* Free the rx chain by slot (the pcb is gone), fail the posted send, mark the slot dead (its
     * pcb cleared, so lwr_conn_find can't alias a reuse of the address). */
    lwr_mark_dead(c);

    if (c->pend_accept && c->owner == NULL) {
        /* Aborted in the accept->post_recv window: the ACCEPT was never surfaced to the driver,
         * so there is no KlHttpConn to notify; recycle the slot silently rather than surfacing a
         * bogus ACCEPT on a freed pcb. */
        lwr_slot_clear(c);
    }
    /* Otherwise every op the driver has posted completes as a failure: the send's WRITE was just
     * failed, an armed recv's READ fails through kl_lwr_next_readable. The slot is cleared when
     * the driver closes the handle. */
}

/* accept callback: reserve a slot, arm the conn callbacks, mark ACCEPT pending. If the slot
 * table is full (accepts beyond conn_cap), tcp_abort the new pcb, rejected, never represented
 * (mirrors the completion contract: no accepted-but-unrepresentable pcb). */
static err_t lwr_srv_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    KlLwrCtx *ctx = arg;   /* the listener's tcp_arg is the ctx */
    if (err != ERR_OK || newpcb == NULL) return ERR_VAL;
    if (!ctx) { tcp_abort(newpcb); return ERR_ABRT; }

    KlLwrConn *cs = lwr_conn_alloc(ctx, newpcb);
    if (!cs) { tcp_abort(newpcb); return ERR_ABRT; }   /* full: reject */

    tcp_recv(newpcb, lwr_srv_recv);
    tcp_sent(newpcb, lwr_srv_sent);
    tcp_err(newpcb, lwr_srv_err);
    /* Key the server callbacks on the SLOT from accept onward: lwr_srv_err
     * resolves the slot directly from `arg`, so an abort (RST/OOM) in the window BEFORE
     * the driver adopts the conn (kl_lwr_set_owner) is still routed to its slot instead
     * of being dropped; a dropped err would leave a dangling pcb the driver would then adopt
     * (a use-after-free). lwr_srv_recv/sent ignore `arg` (they find the slot by pcb). */
    tcp_arg(newpcb, cs);

    cs->pend_accept = 1;
    memcpy(cs->peer_ip, &newpcb->remote_ip.addr, 4);   /* network order (ip4 addr) */
    cs->peer_port = newpcb->remote_port;                /* host order in lwIP */
    return ERR_OK;
}

/* ── outbound client: tcp_connect + connected_cb + client teardown ─────────
 * A client slot reuses the retained-recv queue (lwr_srv_recv) for the response. Its request send
 * is a direct real kl_lwr_client_send (NOT the send-pump) and its completions surface via the
 * client's tagged watcher (KL_LWR_CONNECT for connect; KL_COMP_WATCHER relay for the data plane);
 * never the server-side KL_LWR_WRITE/terminal path (which needs a KlHttpConn the driver owns). So the
 * client uses its OWN err callback (lwr_cli_err), keyed by tcp_arg == the watcher udata. */

/* Find a client slot by its tagged-watcher udata (the err callback's arg after the pcb is freed). */
static KlLwrConn *lwr_client_by_watcher(KlLwrCtx *ctx, const void *watcher) {
    if (!ctx || watcher == NULL) return NULL;
    for (int i = 0; i < ctx->conn_cap; i++)
        if (ctx->conns[i].is_client && ctx->conns[i].watcher_udata == watcher &&
            (ctx->conns[i].pcb != NULL || ctx->conns[i].dead))
            return &ctx->conns[i];
    return NULL;
}

/* client err callback: lwIP has already freed the pcb. Key the slot by the watcher udata (tcp_arg).
 * If the connect had not yet completed, this is a CONNECT FAILURE → surface a failed connect
 * completion (pend_connect, connect_ok=0). If already connected, it is a mid-stream RST → mark the
 * slot closed+dead so the client's next readable relay + kl_lwr_client_recv return EOF (0). Frees
 * the rx chain by owner; clears the live handle so find() can't alias a reused address. */
static void lwr_cli_err(void *arg, err_t err) {
    (void)err;
    KlLwrCtx *ctx = lwr_ctx();
    KlLwrConn *c = lwr_client_by_watcher(ctx, arg);
    if (!c) return;
    lwr_rx_free(c);
    c->dead_fd = c->pcb;
    c->pcb = NULL;
    c->dead = 1;
    c->closed = 1;
    if (!c->connected && !c->pend_connect) {   /* connect failed before connected_cb succeeded */
        c->pend_connect = 1;
        c->connect_ok = 0;
    }
    /* If already connected, the closed+dead flags drive the client's readable relay → recv EOF. */
}

/* connected callback: on ERR_OK mark the slot connected, wire recv/sent, and surface a SUCCESS
 * connect completion. On any error surface a FAILED connect completion (the pcb is torn down by
 * lwIP after we return the error, and lwr_cli_err may also fire; pend_connect is set at most once). */
static err_t lwr_cli_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    (void)arg;
    KlLwrCtx *ctx = lwr_ctx();
    KlLwrConn *c = lwr_conn_find(ctx, tpcb);
    if (!c) { tcp_abort(tpcb); return ERR_ABRT; }   /* no slot (should not happen): abort it, and
                                                     * say so (ERR_ABRT promises a freed pcb) */
    if (err != ERR_OK) {
        if (!c->pend_connect) { c->pend_connect = 1; c->connect_ok = 0; }
        return err;
    }
    c->connected = 1;
    tcp_recv(tpcb, lwr_srv_recv);   /* the retained-recv path is direction-agnostic (response) */
    tcp_sent(tpcb, lwr_srv_sent);   /* harmless for the client (send_active is never set) */
    if (!c->pend_connect) { c->pend_connect = 1; c->connect_ok = 1; }
    return ERR_OK;
}

int kl_lwr_connect(void *lwrctx, void *pcb, const uint8_t ip4[4], uint16_t port,
                   void *owner_watcher) {
    KlLwrCtx *ctx = lwrctx;
    struct tcp_pcb *p = (struct tcp_pcb *)pcb;
    if (!ctx || p == NULL || !ip4) return -1;

    /* Reserve a slot for the client pcb (reuse the accepted-pcb slot machinery). */
    KlLwrConn *c = lwr_conn_alloc(ctx, p);
    if (!c) return -1;                 /* table full: caller closes the pcb */
    c->is_client     = 1;
    c->watcher_udata = owner_watcher;  /* tagged KlWatcher udata: connect + data-plane relay */
    c->owner         = NULL;           /* client slots never carry a KlHttpConn owner */

    tcp_arg(p, owner_watcher);         /* lwr_cli_err receives this (owner-keyed teardown) */
    tcp_err(p, lwr_cli_err);

    ip_addr_t dst;
    IP_ADDR4(&dst, ip4[0], ip4[1], ip4[2], ip4[3]);
    err_t rc = tcp_connect(p, &dst, port, lwr_cli_connected);
    if (rc != ERR_OK) {
        /* Local failure (out of memory / bad args): detach + abort, free the slot. The caller owns
         * closing on -1, but the pcb is already unusable; abort here and clear the slot so no
         * dangling completion is surfaced. */
        tcp_arg(p, NULL);
        tcp_err(p, NULL);
        lwr_slot_clear(c);
        tcp_abort(p);
        return -1;
    }
    return 0;
}

/* ── client data-plane: watcher recording + readiness relay + real send/recv ──────── */

void kl_lwr_client_watch(void *lwrctx, void *pcb, unsigned mask, void *watcher_udata) {
    KlLwrCtx *ctx = lwrctx;
    KlLwrConn *c = lwr_conn_find(ctx, (struct tcp_pcb *)pcb);
    if (!c || !c->is_client) return;
    c->watcher_mask  = mask;
    c->watcher_udata = watcher_udata;
}

void kl_lwr_client_unwatch(void *lwrctx, void *pcb) {
    KlLwrCtx *ctx = lwrctx;
    KlLwrConn *c = lwr_conn_find(ctx, (struct tcp_pcb *)pcb);
    if (c && c->is_client) c->watcher_mask = 0;
}

int kl_lwr_next_client_ready(void *lwrctx, int *cursor, void **watcher_udata, unsigned *mask) {
    KlLwrCtx *ctx = lwrctx;
    if (!ctx || !cursor) return 0;
    for (int i = *cursor; i < ctx->conn_cap; i++) {
        KlLwrConn *c = &ctx->conns[i];
        if (!c->is_client || !c->watcher_mask || c->watcher_udata == NULL) continue;
        unsigned ready = 0;
        /* Readable: rx data queued OR the peer closed/errored (EOF the client must observe). */
        if ((c->watcher_mask & KL_LWR_EV_READ) &&
            (c->rx_queued > 0 || c->closed || c->dead))
            ready |= KL_LWR_EV_READ;
        /* Writable: connected + live + tcp_sndbuf headroom (the client can queue more request). */
        if ((c->watcher_mask & KL_LWR_EV_WRITE) && c->connected && !c->dead && c->pcb != NULL &&
            tcp_sndbuf(c->pcb) > 0)
            ready |= KL_LWR_EV_WRITE;
        if (!ready) continue;
        *cursor = i + 1;
        if (watcher_udata) *watcher_udata = c->watcher_udata;
        if (mask)          *mask          = ready;
        return 1;
    }
    *cursor = ctx->conn_cap;
    return 0;
}

long kl_lwr_client_send(void *lwrctx, void *pcb, const void *buf, size_t len, int *would_block) {
    KlLwrCtx *ctx = lwrctx;
    struct tcp_pcb *p = (struct tcp_pcb *)pcb;
    KlLwrConn *c = lwr_conn_find(ctx, p);
    if (would_block) *would_block = 0;
    if (!c || !c->is_client || !c->connected || c->dead || p == NULL) return -1;
    if (len == 0) return 0;

    u16_t sndbuf = tcp_sndbuf(p);
    if (sndbuf == 0) { if (would_block) *would_block = 1; return 0; }   /* EAGAIN: re-arm WRITE */
    size_t chunk = len;
    if (chunk > sndbuf) chunk = sndbuf;
    if (chunk > KL_LWR_TCP_WRITE_MAX) chunk = KL_LWR_TCP_WRITE_MAX;

    err_t w = tcp_write(p, buf, (u16_t)chunk, TCP_WRITE_FLAG_COPY);
    if (w == ERR_MEM) { if (would_block) *would_block = 1; return 0; }  /* queue full: EAGAIN */
    if (w != ERR_OK) return -1;                                         /* hard error */
    tcp_output(p);
    return (long)chunk;
}

long kl_lwr_client_recv(void *lwrctx, void *pcb, void *dst, size_t cap, int *would_block) {
    KlLwrCtx *ctx = lwrctx;
    struct tcp_pcb *p = (struct tcp_pcb *)pcb;
    KlLwrConn *c = lwr_conn_find(ctx, p);
    if (would_block) *would_block = 0;
    /* A dead/closed slot with no queued data = EOF (0). find() returns NULL for a dead slot (pcb
     * cleared), so look it up by the fd handle too so a post-RST recv reports EOF, not an error. */
    if (!c) {
        KlLwrConn *d = lwr_client_by_fd(ctx, pcb);
        if (d && d->is_client && (d->closed || d->dead)) return 0;   /* EOF */
        return -1;
    }
    if (!c->is_client) return -1;
    if (c->rx_queued > 0)
        return (long)kl_lwr_take_staged(ctx, p, dst, cap);
    if (c->closed || c->dead) return 0;                              /* peer closed: EOF */
    if (c->connected) { if (would_block) *would_block = 1; return -1; }  /* no data yet: EAGAIN */
    if (would_block) *would_block = 1;
    return -1;
}

/* Synchronous send on a server-accepted (non-client) live pcb; see the header. An accepted pcb is
 * already established (post-3WHS), so no `connected` flag is set/needed; require a live slot with a
 * driver-owned KlHttpConn (owner != NULL) that is NOT a client and NOT already running the async send-
 * pump (send_active), so a handshake flush never races the response body pump on the same pcb. */
long kl_lwr_srv_sync_send(void *lwrctx, void *conn, const void *buf, size_t len, int *would_block) {
    KlLwrCtx *ctx = lwrctx;
    KlLwrConn *c = lwr_srv_live(ctx, conn);
    struct tcp_pcb *p = c ? c->pcb : NULL;
    if (would_block) *would_block = 0;
    if (!c || c->send_active || c->owner == NULL || p == NULL) return -1;
    if (len == 0) return 0;

    u16_t sndbuf = tcp_sndbuf(p);
    if (sndbuf == 0) { if (would_block) *would_block = 1; return 0; }   /* no headroom: would-block */
    size_t chunk = len;
    if (chunk > sndbuf) chunk = sndbuf;
    if (chunk > KL_LWR_TCP_WRITE_MAX) chunk = KL_LWR_TCP_WRITE_MAX;

    err_t w = tcp_write(p, buf, (u16_t)chunk, TCP_WRITE_FLAG_COPY);
    if (w == ERR_MEM) { if (would_block) *would_block = 1; return 0; }  /* queue full: would-block */
    if (w != ERR_OK) return -1;                                         /* hard error */
    tcp_output(p);
    return (long)chunk;
}

/* ── UDP datagram glue (udp_pcb ↔ KlDatagram) ──────────────────────────────
 * A udp slot is created by kl_lwr_udp_new (lwr_sock_socket for SOCK_DGRAM), bound by
 * kl_lwr_udp_bind, recv-armed (taking a stable-token ref) by kl_lwr_udp_post_recv, and closed by
 * kl_lwr_udp_close. Inbound datagrams land in lwr_udp_recv_cb, which copies the payload out of the
 * pbuf (freeing it immediately, no retained pbuf) into the bounded per-slot ring. The drain
 * (kl_lwr_udp_drain) surfaces one queued datagram per armed slot as a UDP-RECV record, and one
 * pending send per slot as a UDP-SEND record. */

/* Find a udp slot by its live pcb. NULL if none. */
static KlLwrUdpSlot *lwr_udp_find(KlLwrCtx *ctx, const struct udp_pcb *pcb) {
    if (!ctx || pcb == NULL) return NULL;
    for (int i = 0; i < KL_LWR_UDP_SLOTS; i++)
        if (ctx->udp[i].pcb == pcb) return &ctx->udp[i];
    return NULL;
}

/* Reserve a free udp slot for a fresh pcb (zero-initialised). NULL if the table is full. */
static KlLwrUdpSlot *lwr_udp_alloc(KlLwrCtx *ctx, struct udp_pcb *pcb) {
    for (int i = 0; i < KL_LWR_UDP_SLOTS; i++)
        /* A slot with a pending cancel-terminal (udp_term[i] != NULL) is NOT reusable until that
         * terminal drains; the terminal is tied to slot index i, so reusing i would collide with (and
         * silently drop) the queued terminal's ref. This is what keeps udp_term bounded 1:1 to slots. */
        if (ctx->udp[i].pcb == NULL && ctx->udp_term[i] == NULL) {
            memset(&ctx->udp[i], 0, sizeof(ctx->udp[i]));
            ctx->udp[i].pcb = pcb;
            return &ctx->udp[i];
        }
    return NULL;
}

/* lwIP udp_recv callback: a datagram arrived on `pcb`. ONE-HELD-PACKET contract: capture a datagram
 * ONLY when a recv is armed AND none is already held. Drop (free the pbuf) otherwise:
 *   - NOT armed (`!rx_armed`): no posted recv op wants it; a completion backend with no socket receive
 *     buffer must not buffer datagrams arriving before recv_start, after recv_stop, or in any unarmed
 *     gap, else they would be delivered STALE when a later recv is posted;
 *   - already held (`has_held`): a burst under a held datagram is deterministic UDP loss, never buffered.
 * The udp_recv callback stays wired for the socket's whole life (detached only on close), so `rx_armed`
 * (not the callback wiring) is the authoritative "a recv is posted" gate. On capture, copy the payload
 * out of the pbuf chain (bounded to KL_LWR_UDP_DGRAM_MAX; truncate + flag if larger) into the single held
 * slot, record the source IPv4 + port, and free the pbuf (lwIP hands us ownership). Runs inline on the
 * mainloop tick (NO_SYS=1 single-thread), so no locking. IPv4-only (loopif). */
static void lwr_udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                            const ip_addr_t *addr, u16_t port) {
    (void)arg; (void)pcb;
    KlLwrCtx *ctx = lwr_ctx();
    KlLwrUdpSlot *s = lwr_udp_find(ctx, pcb);
    if (!s || p == NULL) { if (p) pbuf_free(p); return; }

    if (!s->rx_armed || s->has_held) { pbuf_free(p); return; }   /* unarmed, or one already held → drop */
    KlLwrDgram *d = &s->held;

    u16_t want = p->tot_len;
    d->truncated = 0;
    if (want > KL_LWR_UDP_DGRAM_MAX) { want = KL_LWR_UDP_DGRAM_MAX; d->truncated = 1; }
    u16_t got = pbuf_copy_partial(p, d->data, want, 0);
    d->len = got;

    /* Source address: IPv4 network-order bytes + host-order port. lwIP stores ip4 addr in
     * network order already (addr->addr / u_addr.ip4.addr). */
    if (addr && IP_IS_V4(addr)) {
        u32_t a = ip4_addr_get_u32(ip_2_ip4(addr));
        memcpy(d->src_ip, &a, 4);
    } else {
        memset(d->src_ip, 0, 4);
    }
    d->src_port = port;

    s->has_held = 1;
    pbuf_free(p);   /* payload copied out: free the pbuf now (no retained pbuf) */
}

void *kl_lwr_udp_new(void) {
    struct udp_pcb *p = udp_new();
    if (!p) return NULL;
    KlLwrCtx *ctx = lwr_ctx();
    if (!ctx) { udp_remove(p); return NULL; }
    KlLwrUdpSlot *s = lwr_udp_alloc(ctx, p);
    if (!s) { udp_remove(p); return NULL; }   /* udp-slot table full */
    return p;
}

int kl_lwr_is_udp(void *lwrctx, void *pcb) {
    KlLwrCtx *ctx = lwrctx;
    return lwr_udp_find(ctx, (const struct udp_pcb *)pcb) != NULL;
}

int kl_lwr_udp_bind(void *pcb, const uint8_t ip4[4], uint16_t port) {
    ip_addr_t addr;
    if (!ip4 || (ip4[0] | ip4[1] | ip4[2] | ip4[3]) == 0) {
        addr = *IP_ADDR_ANY;
    } else {
        IP_ADDR4(&addr, ip4[0], ip4[1], ip4[2], ip4[3]);
    }
    return udp_bind((struct udp_pcb *)pcb, &addr, port) == ERR_OK ? 0 : -1;
}

uint16_t kl_lwr_udp_local_port(void *pcb) {
    return ((struct udp_pcb *)pcb)->local_port;
}

/* Arm one recv on a udp pcb (mirrors kl_lwr_conn_arm), taking a stable-token ref for the posted op.
 * Wires the udp_recv callback on first arm (idempotent). Returns 0, or -1 if the pcb has no slot. */
int kl_lwr_udp_post_recv(void *lwrctx, void *pcb, void *life) {
    KlLwrCtx *ctx = lwrctx;
    KlLwrUdpSlot *s = lwr_udp_find(ctx, (const struct udp_pcb *)pcb);
    if (!s) return -1;
    s->life = (KlCompLife *)life;
    if (!s->rx_armed) {                 /* arm takes ONE token ref; the drain transfers it out */
        kl_comp_life_retain(s->life);
        s->rx_armed = 1;
    }
    udp_recv((struct udp_pcb *)pcb, lwr_udp_recv_cb, NULL);
    return 0;
}

/* Send one datagram out `pcb` to dest ip4:port via udp_sendto. Copies `data` into a fresh pbuf,
 * sends, frees it (a datagram is one pbuf, the only alloc lwIP requires). Records a pending
 * KL_COMP_DGRAM_SEND on the slot so the drain reports it (with the byte count). Returns 0 / -1. */
int kl_lwr_udp_send(void *lwrctx, void *pcb, void *life, const void *data, size_t len,
                    const uint8_t dest_ip[4], uint16_t dest_port) {
    KlLwrCtx *ctx = lwrctx;
    struct udp_pcb *p = (struct udp_pcb *)pcb;
    KlLwrUdpSlot *s = lwr_udp_find(ctx, p);
    if (!s || !p || len > 0xffffu) return -1;
    s->life = (KlCompLife *)life;   /* the send may be the first op that names the token */

    struct pbuf *pb = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!pb) return -1;
    if (len > 0) {
        if (pbuf_take(pb, data, (u16_t)len) != ERR_OK) { pbuf_free(pb); return -1; }
    }

    ip_addr_t dst;
    if (dest_ip)
        IP_ADDR4(&dst, dest_ip[0], dest_ip[1], dest_ip[2], dest_ip[3]);
    else
        dst = *IP_ADDR_ANY;
    err_t rc = udp_sendto(p, pb, &dst, dest_port);
    pbuf_free(pb);
    if (rc != ERR_OK) return -1;

    /* Record the completed send (bounded FIFO of byte counts). On overflow (a burst larger than
     * the ring between drains) fold into the tail so no accounting is lost; udp.c's on_send just
     * releases the outstanding-bytes reservation, so a coalesced count is correct there. */
    if (s->pend_send < KL_LWR_UDP_SEND_RING) {
        int idx = (s->send_head + s->pend_send) % KL_LWR_UDP_SEND_RING;
        s->send_len[idx] = len;
        s->pend_send++;
        kl_comp_life_retain(s->life);   /* new pending completion → ONE token ref (drain transfers it) */
    } else {
        /* Coalesced into an existing pending record: no NEW record, so no new ref (the folded record
         * still carries exactly one ref). Keeps refs == outstanding records == future transfers. */
        int tail = (s->send_head + s->pend_send - 1) % KL_LWR_UDP_SEND_RING;
        s->send_len[tail] += len;
    }
    return 0;
}

void kl_lwr_udp_close(void *lwrctx, void *pcb) {
    KlLwrCtx *ctx = lwrctx;
    struct udp_pcb *p = (struct udp_pcb *)pcb;
    if (p == NULL) return;
    KlLwrUdpSlot *s = lwr_udp_find(ctx, p);
    if (!s) return;                 /* not ours / already closed: idempotent no-op */
    /* Release every token ref the drain never transferred out: the armed recv (if any) + each
     * pending send. Balances the retains in post_recv/send; the token's final release (once the owner
     * ref is also gone) frees the receive storage. life == NULL (slot never posted) → no-op. */
    for (int k = (s->rx_armed ? 1 : 0) + s->pend_send; k > 0; k--)
        kl_comp_life_release(s->life);
    udp_recv(p, NULL, NULL);        /* detach the recv callback */
    udp_remove(p);                  /* free the pcb */
    /* Discard any held datagram unconditionally. A held datagram CAN outlive a drain (an armed slot may
     * be left holding one if the drain's output budget `max` is exhausted before this slot is visited),
     * so close must not assume has_held == 0. It is safe regardless: the held datagram is inline in the
     * slot (no heap) and carries NO token ref (only rx_armed/pend_send do, released above), so clearing
     * it is a plain memset with nothing to free. */
    memset(s, 0, sizeof(*s));       /* clears pcb (→ free slot) + any held datagram */
}

/* Cancel the armed recv for `life` → a context-owned pending terminal (survives slot teardown).
 * See lwip_raw_glue.h. No allocation; idempotent. */
void kl_lwr_udp_cancel_recv(void *lwrctx, void *life) {
    KlLwrCtx *ctx = lwrctx;
    if (!ctx || !life) return;
    KlCompLife *l = (KlCompLife *)life;
    for (int i = 0; i < KL_LWR_UDP_SLOTS; i++) {
        KlLwrUdpSlot *s = &ctx->udp[i];
        if (s->pcb == NULL || s->life != l || !s->rx_armed) continue;
        /* The terminal is tied to THIS slot index i: a genuinely bounded 1:1 mapping (one terminal per
         * udp slot), so udp_term can never overflow: lwr_udp_alloc will not reuse slot i until
         * udp_term[i] drains. The originating entry MUST be free here (a slot with a pending terminal is
         * never re-armed). If it is unexpectedly occupied, that is an invariant violation; do NOT clear
         * the arm (no silent ref loss); leave the arm so close/teardown releases its ref. */
        if (ctx->udp_term[i] != NULL) return;   /* invariant failure: keep the arm, drop nothing */
        s->rx_armed = 0;            /* remove the arm so a held datagram can no longer complete */
        s->has_held = 0;            /* discard any held datagram (inline in the slot, no ref, no free) */
        ctx->udp_term[i] = l;       /* TRANSFER the arm's one token ref into THIS slot's terminal */
        return;                     /* one armed recv per slot; one token ↔ one slot */
    }
    /* No armed recv for `life` (already cancelled / drained / never armed) → idempotent no-op. */
}

/* 1 while a pending recv terminal is queued for `life`, else 0. See lwip_raw_glue.h. */
int kl_lwr_udp_recv_pending(void *lwrctx, void *life) {
    KlLwrCtx *ctx = lwrctx;
    if (!ctx || !life) return 0;
    for (int t = 0; t < KL_LWR_UDP_SLOTS; t++)
        if (ctx->udp_term[t] == (KlCompLife *)life) return 1;
    return 0;
}

/* Drain: surface up to `max` UDP completions. Per armed slot with a queued datagram, emit ONE
 * UDP-RECV (the oldest, copied into `staged` so buf stays valid this drain), consuming the arm.
 * Per slot with pending sends, emit UDP-SEND records. Bounded by KL_LWR_UDP_SLOTS * ring. */
int kl_lwr_udp_drain(void *lwrctx, KlLwrUdpRecord *out, int max) {
    KlLwrCtx *ctx = lwrctx;
    if (!ctx || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < KL_LWR_UDP_SLOTS && n < max; i++) {
        KlLwrUdpSlot *s = &ctx->udp[i];
        if (s->pcb == NULL) continue;

        /* One recv per armed slot (one-in-flight / one-held recv contract). */
        if (s->rx_armed && s->has_held && n < max) {
            s->staged = s->held;
            s->has_held = 0;
            s->rx_armed = 0;   /* consumed: the datagram core re-posts via its completion dispatch */

            KlLwrUdpRecord *r = &out[n++];
            memset(r, 0, sizeof(*r));
            r->kind = KL_LWR_DGRAM_RECV;
            r->life = s->life;   /* TRANSFER the arm's token ref → event (rx_armed just cleared) */
            r->data = s->staged.data;
            r->len = s->staged.len;
            r->truncated = s->staged.truncated;
            memcpy(r->src_ip, s->staged.src_ip, 4);
            r->src_port = s->staged.src_port;
        }

        /* Pending sends (each a completed KL_COMP_DGRAM_SEND). */
        while (s->pend_send > 0 && n < max) {
            size_t bytes = s->send_len[s->send_head];
            s->send_head = (s->send_head + 1) % KL_LWR_UDP_SEND_RING;
            s->pend_send--;
            KlLwrUdpRecord *r = &out[n++];
            memset(r, 0, sizeof(*r));
            r->kind = KL_LWR_DGRAM_SEND;
            r->life = s->life;   /* TRANSFER one pending send's token ref → event */
            r->len = bytes;
        }
    }
    /* Emit context-owned pending recv terminals (cancelled armed recvs). One terminal per entry,
     * transferring the token ref → event (the dispatch retires recv_inflight + releases the ref). */
    for (int t = 0; t < KL_LWR_UDP_SLOTS && n < max; t++) {
        if (ctx->udp_term[t] == NULL) continue;
        KlLwrUdpRecord *r = &out[n++];
        memset(r, 0, sizeof(*r));
        r->kind = KL_LWR_DGRAM_RECV;
        r->terminal = 1;                 /* ok=0, no data: retires the recv machine */
        r->life = ctx->udp_term[t];      /* TRANSFER the ref → event */
        ctx->udp_term[t] = NULL;
    }
    return n;
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
    if (lwr_is_handle(pcb)) {                 /* an accepted connection: its live slot's pcb */
        KlLwrConn *c = lwr_srv_live(lwr_ctx(), pcb);
        return c ? c->pcb->local_port : 0;
    }
    return ((struct tcp_pcb *)pcb)->local_port;
}

void kl_lwr_tcp_close(void *lwrctx, void *pcb) {
    KlLwrCtx *ctx = lwrctx;
    struct tcp_pcb *p = (struct tcp_pcb *)pcb;
    if (p == NULL) return;

    /* An accepted connection, by slot handle. A dead slot (its pcb already freed) is only
     * cleared, never dereferenced; a live one is closed. A stale handle (its slot cleared, or
     * taken by a later accept since) matches nothing: a no-op, never another connection. */
    if (lwr_is_handle(pcb)) {
        KlLwrConn *c = lwr_srv_slot(ctx, pcb);
        if (!c) return;
        if (c->dead) { lwr_slot_clear(c); return; }
        p = c->pcb;
        lwr_slot_clear(c);
        tcp_arg(p, NULL);
        tcp_recv(p, NULL);
        tcp_sent(p, NULL);
        tcp_err(p, NULL);
        /* tcp_close may fail (data still queued); lwIP REQUIRES a tcp_abort fallback then. */
        if (tcp_close(p) != ERR_OK) tcp_abort(p);
        return;
    }

    /* A client pcb by pointer: a LIVE client slot (->pcb == p) is preferred; otherwise a DEAD one
     * whose ->dead_fd == p (tcp_err already freed the pcb; the client still holds the old pointer).
     * For a dead slot we ONLY clear the slot, never dereference the freed pcb. */
    KlLwrConn *slot = lwr_client_by_fd(ctx, p);
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
     * this fd was already torn down (its slot cleared); closing again would re-enter tcp_close on
     * an already-closed/TIME-WAIT pcb and corrupt lwIP's TCP lists (the "TIME-WAIT pcb->state"
     * assertion). So an untracked close is an idempotent no-op; the exactly-once close discipline
     * lives in the slot lifetime, not in repeated tcp_close calls. */
    if (!slot) return;

    lwr_slot_clear(slot);             /* frees rx chain + owned send buffer, clears the slot */
    tcp_arg(p, NULL);
    if (p->state != LISTEN) {         /* connection pcb: detach the conn callbacks */
        tcp_recv(p, NULL);
        tcp_sent(p, NULL);
        tcp_err(p, NULL);
    }
    /* tcp_close may fail (data still queued); lwIP REQUIRES a tcp_abort fallback then. */
    if (tcp_close(p) != ERR_OK)
        tcp_abort(p);
}

void kl_lwr_tcp_abort(void *lwrctx, void *pcb) {
    KlLwrCtx *ctx = lwrctx;
    struct tcp_pcb *p = (struct tcp_pcb *)pcb;
    if (p == NULL || (ctx && p == ctx->listen_pcb)) return;

    /* An accepted connection, by slot handle: abort it if still live. Its outstanding ops then
     * complete as failures (the posted send's WRITE, an armed recv's READ). A dead slot needs
     * nothing more: its ops already have their failed completions owed, and an armed recv's READ
     * surfaces through kl_lwr_next_readable however often this is called. Stale: a no-op. */
    if (lwr_is_handle(pcb)) {
        KlLwrConn *c = lwr_srv_live(ctx, pcb);
        if (c) lwr_srv_kill(c);
        return;
    }

    /* A client pcb by pointer. */
    KlLwrConn *slot = lwr_conn_find(ctx, p);
    /* Already gone: an idempotent no-op. A stale client pointer can name an accepted server pcb
     * that reused the address (lwIP's pools are LIFO): only a client slot is aborted here. */
    if (!slot || slot->dead || !slot->is_client) return;
    lwr_mark_dead(slot);               /* keeps a dead_fd copy for the client's close / EOF */
    tcp_arg(p, NULL);
    tcp_recv(p, NULL);
    tcp_sent(p, NULL);
    tcp_err(p, NULL);
    tcp_abort(p);                      /* frees the pcb + RST */
}

void kl_lwr_set_owner(void *lwrctx, void *conn, void *owner) {
    KlLwrCtx *ctx = lwrctx;
    KlLwrConn *cs = lwr_srv_slot(ctx, conn);
    if (cs) cs->owner = owner;
    /* Do NOT tcp_arg(p, owner): the server pcb's arg stays the SLOT (set in lwr_srv_accept),
     * so lwr_srv_err always resolves its slot even before the owner is set. The
     * terminal-completion target is cs->owner, tracked here; the err callback does not need
     * arg to be the owner. */
}

int kl_lwr_conn_arm(void *lwrctx, void *conn, void *buf, size_t cap) {
    KlLwrCtx *ctx = lwrctx;
    /* Live only: a dead conn takes no new op (the post fails, and the driver closes it). */
    KlLwrConn *cs = lwr_srv_live(ctx, conn);
    if (!cs) return -1;   /* no slot: the backend must not leave this conn accepted-but-mute */
    cs->armed    = 1;
    cs->recv_buf = buf;   /* raw destination the completion driver chose for this recv */
    cs->recv_cap = cap;
    return 0;
}

void kl_lwr_conn_disarm(void *lwrctx, void *conn) {
    KlLwrCtx *ctx = lwrctx;
    KlLwrConn *cs = lwr_srv_slot(ctx, conn);
    if (cs) cs->armed = 0;
}

int kl_lwr_next_readable(void *lwrctx, int *cursor, void **owner, void **conn,
                         void **recv_buf, size_t *recv_cap, int *closed) {
    KlLwrCtx *ctx = lwrctx;
    if (!ctx || !cursor) return 0;
    for (int i = *cursor; i < ctx->conn_cap; i++) {
        KlLwrConn *c = &ctx->conns[i];
        if (c->owner == NULL || !c->armed || c->is_client) continue;
        /* An armed recv completes with data, or (the peer closed and every retained byte was
         * delivered, or the conn died) as a failed READ. One READ per armed recv: the backend
         * disarms on delivery, so nothing is reported twice, and nothing else (a failed WRITE,
         * a cancel) can stop the armed recv from completing. */
        int has_data = (!c->dead && c->rx_queued > 0);
        int is_closed = (c->closed || c->dead);
        if (!has_data && !is_closed) continue;
        *cursor = i + 1;   /* advance past this slot for the next call */
        if (owner)    *owner    = c->owner;
        if (conn)     *conn     = lwr_handle_of(ctx, c);
        if (recv_buf) *recv_buf = c->recv_buf;   /* the driver-chosen raw destination */
        if (recv_cap) *recv_cap = c->recv_cap;
        if (closed)   *closed   = has_data ? 0 : 1;
        return 1;
    }
    *cursor = ctx->conn_cap;
    return 0;
}

/* Copy up to `cap` received bytes from the retained pbuf chain into `dst`, then dequeue exactly
 * that many bytes from the FRONT of the chain and ack them. Uses lwIP's own chain primitives so
 * pbuf refcounts / tot_len / the pool free-list stay consistent (a hand-rolled per-pbuf detach
 * is fragile; freeing individual segments of a pbuf_cat'ed chain can corrupt the pool):
 *   - pbuf_copy_partial(rx_head, dst, n, 0)  : copy the first n bytes across pbuf boundaries.
 *   - pbuf_free_header(rx_head, n)           : remove n bytes from the front, freeing fully-
 *                                              consumed head pbufs and returning the new head
 *                                              (a partial head is kept, payload/len adjusted).
 *   - tcp_recved(pcb, n)                     : ack EXACTLY the delivered bytes (fix #1: never
 *                                              ack a byte before it is delivered into read_buf).
 * `cap` is the free room in read_buf (<= read_cap <= header window), which fits u16_t. */
size_t kl_lwr_take_staged(void *lwrctx, void *conn, void *dst, size_t cap) {
    KlLwrCtx *ctx = lwrctx;
    /* A server conn by handle, or a client by its pcb (kl_lwr_client_recv). */
    KlLwrConn *cs = lwr_is_handle(conn) ? lwr_srv_live(ctx, conn)
                                        : lwr_conn_find(ctx, (struct tcp_pcb *)conn);
    struct tcp_pcb *p = cs ? cs->pcb : NULL;
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

/* ── bounded, zero-allocation buffered + file send (reference in place) ─────
 *
 * Store a bounded COPY of the iov ARRAY into the slot (pointers + lengths). Small segments whose
 * data pointer may be transient (the driver's stack Content-Length scratch) are snapshotted into
 * the slot's preallocated head buffer; larger segments (body / large header block, all owned by
 * the live KlHttpResponse) are referenced in PLACE. This copies at most KL_LWR_TX_HEAD bytes of tiny
 * head data. NEVER the payload. Returns 0 on installed, -1 on an iov-count / snapshot-overflow
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
/* Called outside any lwIP callback (from a post), so a pump that aborts the pcb needs no ERR_ABRT
 * here: the send was posted, and it completes as a failed WRITE. */
static void lwr_send_start(struct tcp_pcb *p, KlLwrConn *cs) {
    cs->send_posted = 1;             /* from here on this send owes exactly one WRITE */
    if (cs->send_total == 0) {       /* nothing to send: synthesize an immediate completion */
        cs->pend_write = 1;
        cs->pend_write_ok = 1;
        cs->pend_write_bytes = 0;
        lwr_send_reset(cs);
        return;
    }
    cs->send_active = 1;
    (void)lwr_send_pump(p, cs);
}

int kl_lwr_send_begin(void *lwrctx, void *conn, const KlLwrIoVec *iov, int iovcnt) {
    KlLwrCtx *ctx = lwrctx;
    KlLwrConn *cs = lwr_srv_live(ctx, conn);   /* a dead conn takes no new op */
    if (!cs) return -1;
    struct tcp_pcb *p = cs->pcb;
    lwr_send_reset(cs);              /* one send in flight per conn (completion contract) */
    size_t total = 0;
    if (lwr_store_iov(ctx, cs, iov, iovcnt, &total) != 0) { lwr_send_reset(cs); return -1; }
    cs->is_file    = 0;
    cs->send_total = total;
    lwr_send_start(p, cs);
    return 0;
}

int kl_lwr_sendfile_begin(void *lwrctx, void *conn, const KlLwrIoVec *head, int head_n,
                          int file_fd, uint64_t count) {
    KlLwrCtx *ctx = lwrctx;
    KlLwrConn *cs = lwr_srv_live(ctx, conn);   /* a dead conn takes no new op */
    if (!cs) return -1;
    struct tcp_pcb *p = cs->pcb;
    lwr_send_reset(cs);
    size_t head_total = 0;
    if (lwr_store_iov(ctx, cs, head, head_n, &head_total) != 0) { lwr_send_reset(cs); return -1; }
    if (count > (uint64_t)(SIZE_MAX - head_total)) { lwr_send_reset(cs); return -1; }  /* overflow */
    cs->is_file     = 1;
    cs->file_fd     = file_fd;       /* borrowed: the response layer owns closing it (no dup) */
    cs->file_count  = count;
    cs->head_total  = head_total;
    cs->send_total  = head_total + (size_t)count;
    lwr_send_start(p, cs);
    return 0;
}

void kl_lwr_send_release(void *lwrctx, void *conn) {
    KlLwrCtx *ctx = lwrctx;
    KlLwrConn *cs = lwr_srv_slot(ctx, conn);
    if (cs) lwr_send_reset(cs);
}

/* ── drain: scan all slots, emit pending completions (fix #4) ───────────────────
 * Per slot, emit in order: ACCEPT, then WRITE (ok or failed). Bounded by conn_cap (`max`
 * caps how many the caller's buffer holds this pass; the rest stay pending for the next
 * drain; nothing is lost). READ is NOT emitted here (surfaced from the rx queue by the
 * backend's armed-conn loop). */
int kl_lwr_drain(void *lwrctx, KlLwrRecord *out, int max) {
    KlLwrCtx *ctx = lwrctx;
    if (!ctx || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < ctx->conn_cap && n < max; i++) {
        KlLwrConn *c = &ctx->conns[i];
        if (c->pcb == NULL && !c->dead) continue;   /* free slot */

        /* Client slots surface ONLY the connect completion here; their data plane rides the
         * watcher relay (kl_lwr_next_client_ready) + real send/recv, never the server-side
         * ACCEPT/WRITE/terminal path (which needs a KlHttpConn target). */
        if (c->is_client) {
            if (c->pend_connect && n < max) {
                KlLwrRecord *r = &out[n++];
                memset(r, 0, sizeof(*r));
                r->kind  = KL_LWR_CONNECT;
                r->pcb   = c->pcb;              /* may be NULL if a pre-connect err freed it */
                r->owner = c->watcher_udata;    /* the tagged KlWatcher udata → KL_COMP_CONNECT */
                r->ok    = c->connect_ok;
                c->pend_connect = 0;
            }
            continue;
        }

        if (c->pend_accept && n < max) {
            KlLwrRecord *r = &out[n++];
            memset(r, 0, sizeof(*r));
            r->kind = KL_LWR_ACCEPT;
            r->accepted = lwr_handle_of(ctx, c);   /* the conn's KlSocketHandle: never the pcb */
            r->ok = 1;
            memcpy(r->peer_ip, c->peer_ip, 4);
            r->peer_port = c->peer_port;
            c->pend_accept = 0;
        }
        /* The posted send's single WRITE: ok=1 once fully acked, ok=0 if the conn died (or the
         * peer closed) first. A failed WRITE completes the send op only: an armed recv on the
         * same conn still gets its own (failed) READ, so the driver, which counts its posted
         * ops, sees every one of them complete. */
        if (c->pend_write && n < max) {
#ifndef NDEBUG
            assert(c->owner != NULL && "a posted send has an owner");
#endif
            KlLwrRecord *r = &out[n++];
            memset(r, 0, sizeof(*r));
            r->kind = KL_LWR_WRITE;
            r->pcb = lwr_handle_of(ctx, c);
            r->owner = c->owner;
            r->nbytes = c->pend_write_ok ? c->pend_write_bytes : 0;
            r->ok = c->pend_write_ok;
            c->pend_write = 0;
            c->pend_write_ok = 0;
            c->pend_write_bytes = 0;
            c->send_posted = 0;
        }
    }
    return n;
}
