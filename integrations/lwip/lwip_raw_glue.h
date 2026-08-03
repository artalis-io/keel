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
 * SPDX-License-Identifier: MIT
 */
#ifndef KEEL_LWIP_RAW_GLUE_H
#define KEEL_LWIP_RAW_GLUE_H

#include <stddef.h>   /* size_t */
#include <stdint.h>   /* uint8_t / uint16_t */

/* ── lifecycle / mainloop ──────────────────────────────────────────────────── */

/* Bring up NO_SYS=1 lwIP (once per process) and attach the loopback netif; return the
 * loop netif as an opaque handle, or NULL if the build created no loopif. Idempotent:
 * a second call reuses the already-initialised stack and returns the same netif. */
void *kl_lwr_lwip_up(void);

/* Drive one lwIP mainloop tick: sys_check_timeouts() (lwIP timers) + netif_poll(loopif)
 * (drain the loopback TX queue into RX so raw tcp_* callbacks fire). `loopif` is the
 * opaque handle from kl_lwr_lwip_up(); a NULL handle only fires the timers. */
void kl_lwr_lwip_tick(void *loopif);

/* ── completion-record kinds surfaced to the backend (neutral mirror of KlCompKind) ──
 * ACCEPT + WRITE are enqueued as records (their timing coincides with a completed op the
 * driver initiated). READS are NOT records: raw recv is passive and can race ahead of the
 * driver's kl_comp_post_recv (a client that sends immediately delivers data in the SAME tick
 * as the accept, before the driver has posted a recv). So received bytes accumulate in a
 * per-pcb staging buffer, and the backend pulls a READ only once the owning conn is armed
 * (kl_lwr_conn_status) — decoupling recv delivery from the callback's timing. */
typedef enum {
    KL_LWR_ACCEPT,   /* a new connection was accepted (listen pcb's tcp_accept cb) */
    KL_LWR_WRITE,    /* a posted send was acknowledged sent (tcp_sent cb) */
} KlLwrKind;

/* One finished ACCEPT/WRITE op, drained by the backend and translated into a
 * KlCompletionEvent. `pcb`/`accepted` are opaque tcp_pcb*; `owner` is the KlConn* the backend
 * stored via kl_lwr_set_owner (WRITE), or NULL for a fresh ACCEPT (the backend attaches the
 * new conn then). Addresses are raw IPv4 bytes + host-order port (peer for ACCEPT). */
typedef struct {
    KlLwrKind kind;
    void     *pcb;         /* the connection pcb (WRITE) */
    void     *accepted;    /* ACCEPT: the newly accepted pcb */
    void     *owner;       /* KlConn* set via kl_lwr_set_owner (WRITE) */
    size_t    nbytes;      /* WRITE: bytes acked */
    int       ok;          /* WRITE ok flag */
    uint8_t   peer_ip[4];  /* ACCEPT: peer IPv4 (network order) */
    uint16_t  peer_port;   /* ACCEPT: peer port (host order) */
} KlLwrRecord;

/* ── socket-provider primitives on tcp_pcb (all handles opaque) ────────────── */

/* tcp_new() → opaque listen/conn pcb, or NULL on failure. */
void *kl_lwr_tcp_new(void);
/* tcp_bind(pcb, ip4, port). ip4 NULL / all-zero = IP_ADDR_ANY. Returns 0 / -1. */
int   kl_lwr_tcp_bind(void *pcb, const uint8_t ip4[4], uint16_t port);
/* tcp_listen(pcb): returns the (possibly relocated) listen pcb, or NULL. Also arms the
 * tcp_accept callback so accepts enqueue KL_LWR_ACCEPT records. NOTE: lwIP's tcp_listen
 * frees `pcb` and returns a smaller LISTEN pcb — the caller must adopt the returned
 * handle (the passed-in one is dangling afterwards). */
void *kl_lwr_tcp_listen(void *pcb);
/* pcb->local_port (host order) for a bound pcb — server bound-port readback. */
uint16_t kl_lwr_tcp_local_port(void *pcb);

/* The current LISTEN pcb (the relocated handle from the last kl_lwr_tcp_listen), or NULL.
 * Lets the backend adopt the relocated listen handle after tcp_listen freed the original. */
void *kl_lwr_listen_pcb(void);
/* tcp_close(pcb); best-effort tcp_abort on failure. Detaches callbacks + owner first. */
void  kl_lwr_tcp_close(void *pcb);

/* Associate a KlConn* (opaque owner) with an accepted pcb so recv/sent callbacks tag
 * their records with it, and arm tcp_recv/tcp_sent/tcp_err on that pcb. */
void  kl_lwr_set_owner(void *pcb, void *owner);

/* Report a pcb's receive status for the backend's READ gating: *has_data = staged bytes
 * are waiting; *closed = the peer closed / the connection errored (a zero-length READ the
 * driver turns into a close). Either output may be NULL. */
void kl_lwr_conn_status(void *pcb, int *has_data, int *closed);

/* Copy up to `cap` staged (received) bytes for `pcb` into `dst`; returns the count copied
 * and removes them from the staging buffer. The backend calls this while building a READ
 * event (staging → c->read_buf). tcp_recved was already issued in the recv callback. */
size_t kl_lwr_take_staged(void *pcb, void *dst, size_t cap);

/* tcp_write(pcb, buf, len, COPY) + tcp_output(). Returns bytes accepted (<= len; bounded
 * by tcp_sndbuf), or -1 on a hard error. P9-2: the small single-segment response fits, so
 * a WRITE record follows via tcp_sent. P9-3 handles partial sends / tcp_sndbuf backpressure. */
long  kl_lwr_send(void *pcb, const void *buf, size_t len);

/* ── client (raw-API test peer, like the spike) ────────────────────────────── */

/* tcp_new + tcp_connect to ip4:port; on connect the internal callback sends `req`
 * (len bytes) and captures the response into an internal buffer. Poll it with
 * kl_lwr_client_response(). Returns 0 on the connect being issued, -1 on failure. */
int   kl_lwr_client_start(const uint8_t ip4[4], uint16_t port,
                          const void *req, size_t req_len);
/* Copy the client's captured response so far into `dst` (NUL-terminated if room);
 * returns the byte count. */
size_t kl_lwr_client_response(char *dst, size_t cap);

/* ── drain ─────────────────────────────────────────────────────────────────── */

/* Pull up to `max` finished records into `out`; returns the count (>= 0). */
int   kl_lwr_drain(KlLwrRecord *out, int max);

#endif /* KEEL_LWIP_RAW_GLUE_H */
