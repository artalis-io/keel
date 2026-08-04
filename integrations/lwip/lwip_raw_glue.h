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

/* ── socket-provider primitives on tcp_pcb (all handles opaque) ────────────── */

/* tcp_new() → opaque listen/conn pcb, or NULL on failure. */
void *kl_lwr_tcp_new(void);
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
int   kl_lwr_conn_arm(void *lwrctx, void *pcb);
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
int kl_lwr_next_readable(void *lwrctx, int *cursor, void **owner, void **pcb, int *closed);

/* Copy up to `cap` received bytes for `pcb` into `dst` from the retained pbuf queue; returns
 * the count copied. tcp_recved() is issued HERE (as whole head pbufs are consumed) — never
 * before delivery — and consumed pbufs are pbuf_free'd. The backend calls this while building
 * a READ event (rx queue → c->read_buf). */
size_t kl_lwr_take_staged(void *lwrctx, void *pcb, void *dst, size_t cap);

/* ── full-payload send with backpressure + file send (Stage B — unchanged logic) ─────
 * The completion contract (src/completion.h) says the backend OWNS the bytes for an op's
 * lifetime (copies) and surfaces a SINGLE fully-completed WRITE. A payload larger than
 * tcp_sndbuf is buffered in the glue and pumped across many tcp_sent rounds; only when the
 * LAST byte is acked does a WRITE completion appear. */
int   kl_lwr_send_begin(void *lwrctx, void *pcb, const void *buf, size_t len);
int   kl_lwr_sendfile_begin(void *lwrctx, void *pcb, const void *head, size_t head_len,
                            int file_fd, uint64_t count);
/* Release any per-pcb send buffer for `pcb` (idempotent). */
void  kl_lwr_send_release(void *lwrctx, void *pcb);

/* ── drain ─────────────────────────────────────────────────────────────────── */

/* Scan the ctx's slots and emit up to `max` pending ACCEPT/WRITE/terminal completions into
 * `out`; returns the count (>= 0). Per-slot pending state is bounded by conn_cap, so this can
 * never lose a completion. READs are surfaced separately (kl_lwr_conn_status/take_staged). */
int   kl_lwr_drain(void *lwrctx, KlLwrRecord *out, int max);

#endif /* KEEL_LWIP_RAW_GLUE_H */
