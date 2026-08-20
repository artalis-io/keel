/*
 * lwip_raw_glue.h — INTERNAL seam between the KEEL completion backend and lwIP raw.
 *
 * Why a seam at all: lwIP's headers (lwip/def.h) redefine htons/ntohs as macros and its
 * arch.h typedefs ssize_t — both clash with the host <sys/socket.h>/<netinet/in.h> that
 * KEEL's internal socket seam (src/socket.h → sockcompat.h) and keel/net.h pull in. A
 * single TU cannot include BOTH the lwIP raw headers AND the KEEL socket headers. So the
 * lwIP-touching code lives in lwip_raw_glue.c (lwIP headers ONLY, no KEEL socket headers)
 * and the completion backend lives in event_lwip_raw.c (KEEL headers only, no lwIP). They
 * meet across this tiny, type-neutral header — the "put platform-only helpers behind a
 * seam, keep #ifdef/foreign types out of the shared TU" pattern.
 *
 * The lwIP netif and every tcp_pcb are passed as opaque `void *` so this header pulls no
 * lwIP type. Addresses cross as raw IPv4 bytes + host-order port (marshalled to/from
 * KlSockAddr on the KEEL side) — no `struct sockaddr` (which would drag in a socket header).
 *
 * ── Stage A rewrite (per-context data model) ─────────────────────────────────
 * The backend state was de-globalized: instead of file-scope g_conns[]/g_listen_pcb and a
 * fixed g_recs completion ring, the glue now owns a per-context KlLwrCtx (opaque here) that
 * holds a KlAllocator, the listen pcb, the loop netif, and a conn_cap-sized array of
 * per-connection slots. The backend creates ONE ctx (kl_lwr_ctx_create, taking the
 * allocator + conn_cap derived from KlConfig.max_connections) and threads its opaque handle
 * through every entry point. Under NO_SYS=1 the lwIP core (single loop netif + one timer
 * wheel) is a process-global singleton, so at most one raw ctx can be live at a time: a
 * second concurrent kl_lwr_ctx_create is rejected (returns NULL); sequential
 * create->destroy->create works (the guard clears on destroy).
 *
 * Completions are no longer a shared ring (which could silently drop). Each slot carries
 * its own pending-completion flags (accept / write / terminal); the drain SCANS all slots
 * and emits at most one KlLwrRecord per pending item — bounded by conn_cap, so it can
 * never overflow and a terminal is always deliverable. READs are surfaced from the per-slot
 * retained-pbuf receive queue, not a flag.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef KEEL_LWIP_RAW_GLUE_H
#define KEEL_LWIP_RAW_GLUE_H

#include <stddef.h>   /* size_t */
#include <stdint.h>   /* uint8_t / uint16_t */

/* The glue allocates its per-ctx state through KEEL's KlAllocator (no raw malloc for the NEW
 * Stage-A state). The allocator crosses the seam as an opaque `void *` (cast back to
 * KlAllocator* inside lwip_raw_glue.c) so this header pulls in no KEEL type at all — keeping the
 * seam maximally neutral. */

/* ── per-context lifecycle ─────────────────────────────────────────────────────
 * A KlLwrCtx bundles all per-backend raw state (listen pcb, loop netif, per-conn slots).
 * The lwIP CORE (lwip_init) is still a process-global one-time init, separate from this
 * per-ctx state — see the notes in lwip_raw_glue.c. */

/* Create the per-context backend state. `alloc` is used for ALL glue-owned Stage-A
 * allocations (the slot array); `conn_cap` sizes the per-conn slot table (the authoritative
 * Keel capacity, KlConfig.max_connections). Also brings up NO_SYS=1 lwIP (once per process)
 * and attaches the loopback netif; the returned opaque handle carries the loop netif.
 *
 * Returns an opaque KlLwrCtx* on success, or NULL on failure OR if a raw ctx is already
 * live (NO_SYS=1 forbids two simultaneous raw stacks — sequential create/destroy is fine).
 * `alloc` is a KEEL KlAllocator* passed as an opaque void* across the neutral seam. */
void *kl_lwr_ctx_create(void *alloc, int conn_cap);

/* The opaque loop netif for a ctx (pass to kl_lwr_lwip_tick). NULL-safe. */
void *kl_lwr_ctx_loopif(void *lwrctx);

/* The currently-live raw ctx (the one enforced by the single-active-ctx guard), or NULL. Used
 * by the overlapped socket provider's ops — which are called via a shared static provider with
 * no per-instance context — to reach the ctx for listen/close. Legitimate under NO_SYS=1: at
 * most one ctx is ever live, so this is unambiguous. */
void *kl_lwr_active_ctx(void);

/* Ensure the ctx's slot table holds at least `conn_cap` slots (grow if smaller). Called by the
 * backend at prime time, once the AUTHORITATIVE Keel capacity (KlConfig.max_connections) is
 * known — unifying arm/slot capacity on that single limit. Safe only before any accept (no live
 * slots to relocate). Returns 0 on success, -1 on allocation failure. A no-op if already >=. */
int kl_lwr_ctx_ensure_cap(void *lwrctx, int conn_cap);

/* Destroy a ctx: drain+free every per-conn rx pbuf queue + send buffer, close the listener,
 * clear the active-ctx guard, free the slot array + ctx. Idempotent on NULL. */
void kl_lwr_ctx_destroy(void *lwrctx);

/* ── mainloop ──────────────────────────────────────────────────────────────── */

/* Drive one lwIP mainloop tick: sys_check_timeouts() (lwIP timers) + netif_poll(loopif)
 * (drain the loopback TX queue into RX so raw tcp_* callbacks fire). `loopif` is the
 * opaque handle from kl_lwr_ctx_loopif(); a NULL handle only fires the timers. */
void kl_lwr_lwip_tick(void *loopif);

/* ── completion kinds surfaced to the backend (neutral mirror of KlCompKind) ──
 * ACCEPT + WRITE + terminal are surfaced as completions the drain emits; READ is delivered
 * separately from the per-slot receive queue (kl_lwr_conn_status / kl_lwr_take_staged). */
typedef enum {
    KL_LWR_ACCEPT,   /* a new connection was accepted */
    KL_LWR_WRITE,    /* a posted send completed (ok=1) OR a terminal close (ok=0, nbytes=0) */
    KL_LWR_CONNECT,  /* an outbound connect finished (LC-1): ok=1 connected, ok=0 failed. `owner`
                      * carries the tagged KlWatcher udata the client registered (NOT a KlConn*);
                      * the backend routes it to KL_COMP_CONNECT against that watcher, mask-encoded
                      * (KL_EVENT_WRITE = connected, 0 = failed). See docs/phase10_...design.md LC-1. */
} KlLwrKind;

/* One finished ACCEPT/WRITE (or terminal) op, emitted by kl_lwr_drain and translated into a
 * KlCompletionEvent. `pcb`/`accepted` are opaque tcp_pcb*; `owner` is the KlConn* the backend
 * stored via kl_lwr_set_owner. Addresses are raw IPv4 bytes + host-order port (peer). */
typedef struct {
    KlLwrKind kind;
    void     *pcb;         /* the connection pcb (WRITE / terminal) */
    void     *accepted;    /* ACCEPT: the newly accepted pcb */
    void     *owner;       /* KlConn* set via kl_lwr_set_owner (WRITE / terminal) */
    size_t    nbytes;      /* WRITE: bytes acked */
    int       ok;          /* WRITE ok flag (0 = terminal close) */
    uint8_t   peer_ip[4];  /* ACCEPT: peer IPv4 (network order) */
    uint16_t  peer_port;   /* ACCEPT: peer port (host order) */
} KlLwrRecord;

/* ── LC-3a: UDP datagram completion path (datagram over raw) ──────────────────────────
 * The datagram counterpart of the tcp_* path: a udp_pcb is created + bound + recv-armed via the
 * glue, and its inbound datagrams / completed sends surface as KlLwrUdpRecord's the backend
 * translates into KL_COMP_DGRAM_RECV / KL_COMP_DGRAM_SEND. All lwIP contact stays in the glue;
 * addresses cross as raw IPv4 bytes + host-order port, payloads as byte buffers — no lwIP type. */

typedef enum {
    KL_LWR_DGRAM_RECV,   /* a datagram arrived (data/len + src ip/port) — one per armed recv */
    KL_LWR_DGRAM_SEND,   /* a posted send completed (len bytes) */
} KlLwrUdpKind;

/* One finished UDP op. For UDP_RECV, `data` points into the udp slot's `staged` buffer and stays
 * valid until the NEXT kl_lwr_udp_drain on that ctx (the backend delivers it inline before then).
 * `life` is the stable-liveness token (KlDgramLife*) the op retained at post; the drain TRANSFERS it
 * to the completion event (ev->life), so the backend recovers the owner through the token and never
 * dereferences the possibly-freed transport owner. See src/datagram_life.h + docs/datagram_contract.md §6. */
typedef struct {
    KlLwrUdpKind kind;
    void        *life;        /* KlDgramLife* — token ref transferred op → event */
    const void  *data;        /* UDP_RECV: datagram payload (in the slot's staged buffer) */
    size_t       len;         /* UDP_RECV: payload len / UDP_SEND: bytes sent */
    int          truncated;   /* UDP_RECV: 1 if the datagram was truncated to the buffer */
    int          terminal;    /* UDP_RECV: 1 = a cancelled/terminal recv (ok=0, no data) — 7B-8 */
    uint8_t      src_ip[4];   /* UDP_RECV: source IPv4 (network order) */
    uint16_t     src_port;    /* UDP_RECV: source port (host order) */
} KlLwrUdpRecord;

/* udp_new() → opaque udp_pcb, or NULL (allocation / udp-slot table full). Reserves a udp slot in
 * the active ctx (so kl_lwr_is_udp can later distinguish it from a tcp_pcb at the socket seam). */
void *kl_lwr_udp_new(void);
/* 1 if `pcb` is a udp_pcb the glue tracks (vs a tcp_pcb), else 0. Lets the socket provider route
 * bind/close to the udp path for a datagram handle without pulling an lwIP type. */
int   kl_lwr_is_udp(void *lwrctx, void *pcb);
/* udp_bind(pcb, ip4, port). ip4 NULL / all-zero = IP_ADDR_ANY. Returns 0 / -1. */
int   kl_lwr_udp_bind(void *pcb, const uint8_t ip4[4], uint16_t port);
/* pcb->local_port (host order) for a bound udp pcb — bound-port readback. */
uint16_t kl_lwr_udp_local_port(void *pcb);
/* Arm ONE recv on a udp pcb (wires udp_recv on first call), taking a `life` token reference for the
 * posted op. The drain surfaces one queued datagram per armed slot as KL_LWR_DGRAM_RECV and transfers
 * the ref to it. `life` is the KlDgramLife* (opaque here). Returns 0, -1 if no slot. */
int   kl_lwr_udp_post_recv(void *lwrctx, void *pcb, void *life);
/* Send one datagram out `pcb` to dest ip4:port (udp_sendto; ip4 NULL = ANY). Records a pending
 * KL_LWR_DGRAM_SEND (len bytes) the drain reports, taking a `life` token ref for it. Returns 0 / -1. */
int   kl_lwr_udp_send(void *lwrctx, void *pcb, void *life, const void *data, size_t len,
                      const uint8_t dest_ip[4], uint16_t dest_port);
/* Close a udp pcb: detach the recv cb, udp_remove, free the slot. Idempotent on an unknown/NULL. */
void  kl_lwr_udp_close(void *lwrctx, void *pcb);
/* Scan udp slots + emit up to `max` pending UDP_RECV/UDP_SEND records. Returns the count (>=0). */
int   kl_lwr_udp_drain(void *lwrctx, KlLwrUdpRecord *out, int max);

/* 7B-8: cancel the armed recv for `life` (KlDgramLife*). Removes the arm (so a held datagram can no
 * longer complete) and moves it to a CONTEXT-owned pending-terminal record that SURVIVES kl_lwr_udp_close
 * — the drain later emits ONE terminal KL_LWR_DGRAM_RECV (terminal=1) transferring the arm's token ref,
 * so a KlDatagram completion-close retires recv_inflight. No allocation. Idempotent (a second call, or a
 * life with no armed recv, is a no-op — no duplicate terminal). Only KlDatagram calls this (the removed UDP object never did).
 * does, so its close/teardown path is unchanged. */
void  kl_lwr_udp_cancel_recv(void *lwrctx, void *life);
/* 7B-8: 1 while a pending recv terminal is queued for `life` (retire → PENDING), 0 once it has drained
 * (retire → RETIRED). */
int   kl_lwr_udp_recv_pending(void *lwrctx, void *life);

/* ── socket-provider primitives on tcp_pcb (all handles opaque) ────────────── */

/* tcp_new() → opaque listen/conn pcb, or NULL on failure. */
void *kl_lwr_tcp_new(void);

/* ── outbound connect (LC-1) ──────────────────────────────────────────────────
 * Begin an outbound connect for a CLIENT pcb (the one the client created via kl_lwr_tcp_new /
 * lwr_sock_socket). Reserves a per-conn slot for `pcb` (reusing the accepted-pcb slot machinery
 * so the response rides the SAME retained-recv path and the request the SAME bounded-TX pump),
 * tags tcp_arg with `owner_watcher` (the tagged KlWatcher udata the client registered — carried
 * through the connect callback + err teardown), sets tcp_err, and issues tcp_connect(pcb, &ip4,
 * port, connected_cb). On the later mainloop tick the connected_cb fires:
 *   - err==ERR_OK: mark the slot connected, wire tcp_recv/tcp_sent, enqueue a per-slot
 *     pend_connect (ok=1) — surfaced by kl_lwr_drain as a KL_LWR_CONNECT record.
 *   - err!=ERR_OK (or tcp_err before connect): the pcb is freed by lwIP; enqueue a FAILED
 *     connect completion (ok=0) keyed by owner.
 * `owner_watcher` is an opaque void* (the tagged KlWatcher udata) — no lwIP/KEEL type crosses.
 * Returns 0 if the connect was issued, -1 on a hard failure (no slot / tcp_connect refused) — in
 * which case the caller owns closing the pcb. */
int kl_lwr_connect(void *lwrctx, void *pcb, const uint8_t ip4[4], uint16_t port,
                   void *owner_watcher);

/* Neutral readiness-mask bits crossing the seam for the client data-plane watcher relay (LC-1).
 * They mirror KEEL's KlEventMask (KL_EVENT_READ=1, KL_EVENT_WRITE=2) numerically — a stable public
 * ABI — so the backend can pass a KlEventMask straight through as `unsigned` without pulling a KEEL
 * type into the glue and the glue can test the bits without a KEEL header. Asserted equal to the
 * KlEventMask values on the backend side. */
#define KL_LWR_EV_READ   1u
#define KL_LWR_EV_WRITE  2u

/* Record the armed readiness watcher for a CLIENT pcb (captured by the backend at kl_event add/mod).
 * `mask` is KL_LWR_EV_READ|WRITE; `watcher_udata` is the tagged KlWatcher udata the drain relays as
 * KL_COMP_WATCHER. A no-op (returns without effect) if `pcb` has no slot or is not a client slot —
 * server conns drive I/O via the completion post path, not readiness watchers. */
void kl_lwr_client_watch(void *lwrctx, void *pcb, unsigned mask, void *watcher_udata);
/* Drop the recorded watcher for `pcb` (kl_event del). Idempotent. */
void kl_lwr_client_unwatch(void *lwrctx, void *pcb);

/* Scan client slots for one whose armed readiness watcher condition is met (writable = connected +
 * sndbuf headroom; readable = rx queued or peer-closed). Starts at *cursor (advanced past the hit).
 * Returns 1 and fills *watcher_udata + *mask (the ready subset of the armed mask) for a ready client,
 * or 0 when none remain this pass. The backend relays each as KL_COMP_WATCHER to that watcher. */
int kl_lwr_next_client_ready(void *lwrctx, int *cursor, void **watcher_udata, unsigned *mask);

/* Client data-plane I/O on a connected client pcb (real send/recv, unlike the server's completion
 * post path). Both are non-blocking:
 *   kl_lwr_client_send: tcp_write(COPY) up to `len` bytes bounded by tcp_sndbuf + tcp_output; returns
 *     bytes queued (> 0), 0 if no sndbuf headroom (caller re-arms WRITE = EAGAIN), or -1 on a hard
 *     error / not-connected. Sets *would_block=1 for the 0 (no-headroom) case.
 *   kl_lwr_client_recv: copy up to `cap` bytes from the retained rx queue (tcp_recved acks them);
 *     returns bytes copied (> 0), 0 on peer close/EOF, or -1 with *would_block=1 when connected but
 *     no data yet (caller re-arms READ = EAGAIN). */
long kl_lwr_client_send(void *lwrctx, void *pcb, const void *buf, size_t len, int *would_block);
long kl_lwr_client_recv(void *lwrctx, void *pcb, void *dst, size_t cap, int *would_block);
/* Synchronous send on a SERVER-accepted (non-client) live pcb — the completion-TLS handshake flush
 * (completion_driver.c comp_tls_flush) pushes handshake ciphertext via the socket send op, which on
 * a completion loop with a real fd is a blocking send. The raw backend has no blocking send, so this
 * mirrors kl_lwr_client_send for an accepted pcb: tcp_write(COPY) bounded by tcp_sndbuf + tcp_output.
 * Returns bytes queued (> 0), 0 with *would_block=1 if no sndbuf headroom, or -1 on a hard error /
 * dead pcb / not a server slot. Used ONLY for the small handshake flush (the HTTP response body
 * rides the async completion send-pump). LC-4. */
long kl_lwr_srv_sync_send(void *lwrctx, void *pcb, const void *buf, size_t len, int *would_block);
/* tcp_bind(pcb, ip4, port). ip4 NULL / all-zero = IP_ADDR_ANY. Returns 0 / -1. */
int   kl_lwr_tcp_bind(void *pcb, const uint8_t ip4[4], uint16_t port);
/* tcp_listen(pcb) on `lwrctx`: returns the (possibly relocated) listen pcb, or NULL. Also
 * arms the tcp_accept callback so accepts surface KL_LWR_ACCEPT. NOTE: lwIP's tcp_listen
 * frees `pcb` and returns a smaller LISTEN pcb — the caller must adopt the returned handle
 * (the passed-in one is dangling afterwards). The ctx tracks the relocated listen pcb. */
void *kl_lwr_tcp_listen(void *lwrctx, void *pcb);
/* pcb->local_port (host order) for a bound pcb — server bound-port readback. */
uint16_t kl_lwr_tcp_local_port(void *pcb);

/* The ctx's current LISTEN pcb (the relocated handle from kl_lwr_tcp_listen), or NULL.
 * Lets the backend adopt the relocated listen handle after tcp_listen freed the original. */
void *kl_lwr_listen_pcb(void *lwrctx);
/* tcp_close(pcb) on `lwrctx`; best-effort tcp_abort on failure (data still queued). Frees the
 * slot's rx queue + owned send buffer + detaches callbacks first. If the pcb's slot was marked
 * `dead` by a prior tcp_err (lwIP already freed the pcb), this ONLY clears the slot and never
 * dereferences the freed pointer (no use-after-free on close-after-err). Safe on a NULL pcb. */
void  kl_lwr_tcp_close(void *lwrctx, void *pcb);

/* Forcibly abort a LIVE connection pcb (idle-timeout / kl_comp_cancel) on `lwrctx`. Detaches
 * callbacks first (so lwIP's internal tcp_err is not re-entered against the slot), frees the
 * slot's rx queue + owned send buffer, marks it closed so the backend surfaces the single
 * terminal completion, then tcp_abort. Idempotent + safe on an already-dead/closed/NULL pcb. */
void  kl_lwr_tcp_abort(void *lwrctx, void *pcb);

/* Mark a conn's terminal (close) READ as already surfaced, so a subsequent status check cannot
 * yield a second terminal event (defence against a double comp_close). Idempotent. */
void  kl_lwr_mark_terminated(void *lwrctx, void *pcb);

/* Associate a KlConn* (opaque owner) with an accepted pcb so recv/sent callbacks tag their
 * completions with it, and (re)arm tcp_recv/tcp_sent/tcp_err on that pcb. */
void  kl_lwr_set_owner(void *lwrctx, void *pcb, void *owner);

/* Arm a conn for a single pending recv (the completion contract's one-in-flight recv). Returns
 * 0 on success, non-zero if the pcb has no slot (a slot-less conn must never be left
 * accepted-but-unable-to-receive; the backend closes it). A conn that HAS a slot always arms. */
int   kl_lwr_conn_arm(void *lwrctx, void *pcb, void *buf, size_t cap);
/* Disarm a conn (its READ was surfaced, or it is being torn down). Idempotent. */
void  kl_lwr_conn_disarm(void *lwrctx, void *pcb);

/* Report a pcb's receive status for the backend's armed READ gating: *has_data = retained
 * received bytes are waiting; *closed = the peer closed / errored (a zero-length READ the
 * driver turns into a close). Either output may be NULL. */
void kl_lwr_conn_status(void *lwrctx, void *pcb, int *has_data, int *closed);

/* Find the next ARMED conn that has data waiting or has closed (starting the scan at slot
 * index *cursor, which is advanced past the returned slot). Returns 1 and fills *owner / *pcb /
 * *closed for a ready conn, or 0 when none remain this pass. Lets the backend surface READs by
 * scanning the glue's per-conn slots without seeing any lwIP type — replacing the old
 * backend-side armed table (fix #2, fix #4: bounded by conn_cap, cannot lose a READ). */
int kl_lwr_next_readable(void *lwrctx, int *cursor, void **owner, void **pcb,
                         void **recv_buf, size_t *recv_cap, int *closed);

/* Copy up to `cap` received bytes for `pcb` into `dst` from the retained pbuf queue; returns
 * the count copied. tcp_recved() is issued HERE (as whole head pbufs are consumed) — never
 * before delivery — and consumed pbufs are pbuf_free'd. The backend calls this while building
 * a READ event (rx queue → c->read_buf). */
size_t kl_lwr_take_staged(void *lwrctx, void *pcb, void *dst, size_t cap);

/* ── bounded, zero-allocation send with backpressure + file send (Stage B) ───────────
 * The completion contract (src/completion.h) surfaces a SINGLE fully-completed WRITE per send.
 * Stage B streams a payload of ANY size through a fixed PREALLOCATED per-connection transmit
 * window (KL_LWR_TX_WIN) — NO per-response malloc, NO whole-payload copy, NO whole-file read:
 *   - The slot stores a bounded COPY of the iov ARRAY; the DATA is referenced in PLACE, relying
 *     on the driver keeping the response's serialized segments valid until the KL_COMP_WRITE.
 *     Small (possibly transient) segments are snapshotted into a tiny preallocated head buffer.
 *   - The pump copies min(tcp_sndbuf, KL_LWR_TX_WIN, remaining) bytes into the window and
 *     tcp_write(COPY)s them; tcp_sent advances + re-pumps; ERR_MEM = backpressure.
 *   - A file send preads the file body chunk-by-chunk into the same window (never into memory
 *     whole); a file read error surfaces a FAILED terminal WRITE so the driver closes.
 * Transmit memory is bounded by conn_cap * KL_LWR_TX_WIN, INDEPENDENT of response/file size, so
 * response size is unbounded-by-design.
 *
 * KlLwrIoVec is a type-neutral mirror of the driver's KlIoVec (pointer + length) so the seam
 * pulls in no KEEL socket type — the backend translates KlIoVec -> KlLwrIoVec at the boundary.
 * The `base` pointers must remain valid until the KL_COMP_WRITE (the response-segment lifetime
 * contract) EXCEPT for segments the glue snapshots (any segment <= 256 bytes is snapshotted
 * regardless, covering the driver's transient stack Content-Length scratch). */
typedef struct { const void *base; size_t len; } KlLwrIoVec;

/* Begin a buffered send: `iov`/`iovcnt` (<= 8) describe the already-serialized response. Returns
 * 0 on installed, -1 on failure (no slot / iov-count over the bound / snapshot overflow). */
int   kl_lwr_send_begin(void *lwrctx, void *pcb, const KlLwrIoVec *iov, int iovcnt);
/* Begin a file send: `head`/`head_n` (<= 8) is the serialized response head; then `count` bytes
 * are pread from `file_fd` (borrowed — the response layer owns closing it). Returns 0 / -1. */
int   kl_lwr_sendfile_begin(void *lwrctx, void *pcb, const KlLwrIoVec *head, int head_n,
                            int file_fd, uint64_t count);
/* Reset any in-flight send offsets for `pcb` (idempotent). No heap to release (Stage B). */
void  kl_lwr_send_release(void *lwrctx, void *pcb);

/* ── drain ─────────────────────────────────────────────────────────────────── */

/* Scan the ctx's slots and emit up to `max` pending ACCEPT/WRITE/terminal completions into
 * `out`; returns the count (>= 0). Per-slot pending state is bounded by conn_cap, so this can
 * never lose a completion. READs are surfaced separately (kl_lwr_conn_status/take_staged). */
int   kl_lwr_drain(void *lwrctx, KlLwrRecord *out, int max);

#endif /* KEEL_LWIP_RAW_GLUE_H */
