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
/* tcp_close(pcb); best-effort tcp_abort on failure (data still queued). Detaches callbacks +
 * owner + frees the owned send buffer first (close-with-outstanding). If the pcb's slot was
 * marked `dead` by a prior tcp_err (lwIP already freed the pcb), this ONLY clears the slot and
 * never dereferences the freed pointer (P9-4: no use-after-free on close-after-err). Safe on a
 * NULL pcb. */
void  kl_lwr_tcp_close(void *pcb);

/* P9-4: forcibly abort a LIVE connection pcb (idle-timeout / kl_comp_cancel). Detaches
 * callbacks first (so lwIP's internal tcp_err is not re-entered against the slot), frees the
 * slot + owned send buffer by owner, marks it closed so the backend surfaces the single
 * terminal READ, then tcp_abort (frees the pcb + RSTs the peer). Idempotent + safe on an
 * already-dead/closed/NULL pcb. */
void  kl_lwr_tcp_abort(void *pcb);

/* P9-4: mark a conn's terminal (close) READ as already surfaced, so a subsequent status check
 * cannot yield a second terminal event (defence against a double comp_close). Idempotent. */
void  kl_lwr_mark_terminated(void *pcb);

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

/* ── P9-3: full-payload send with backpressure + file send ────────────────────
 * The completion contract (src/completion.h) says the backend OWNS the bytes for an
 * op's lifetime (copies) and surfaces a SINGLE fully-completed WRITE — never a partial.
 * So a payload larger than tcp_sndbuf must be buffered in the glue and pumped across
 * many tcp_sent rounds; only when the LAST byte is acked does a KL_LWR_WRITE record
 * appear. These functions implement that per-pcb send-buffer state machine.
 *
 * kl_lwr_send_begin(pcb, buf, len): copy `buf`/`len` into the pcb's owned send buffer
 * (replacing any prior — one send in flight per conn, per the completion contract) and
 * kick the first tcp_write round. Returns 0 on success, -1 on error (no send state /
 * OOM / no owning slot). The tcp_sent callback advances the acked offset and pumps the
 * tail; when acked == total, a single KL_LWR_WRITE (owner + total) is enqueued and the
 * send buffer released. Handles tcp_write ERR_MEM (sndbuf full) as backpressure: the
 * round stops and the next tcp_sent resumes it. Works for a many-segment response. */
int   kl_lwr_send_begin(void *pcb, const void *buf, size_t len);

/* kl_lwr_sendfile_begin(pcb, head, head_len, file_fd, count): send `head_len` head
 * bytes followed by `count` bytes read from `file_fd` (at offset 0). P9-3 approach (a):
 * pread the whole file into the same owned send buffer after the head, then reuse the
 * send-pump — bounded by head_len+count (fine for the test's temp file). The glue owns
 * ONLY the read of file_fd; it does NOT close file_fd (the response layer owns that —
 * kl_response_reset/kl_response_free close res->file_fd). Surfaces one KL_LWR_WRITE when
 * the whole head+file is acked. Returns 0 on success, -1 on error (OOM / pread short). */
int   kl_lwr_sendfile_begin(void *pcb, const void *head, size_t head_len,
                            int file_fd, uint64_t count);

/* Release any per-pcb send buffer for `pcb` (frees the owned copy so a conn torn down mid-send
 * does not leak). Idempotent. Retained for completeness; the P9-4 close/abort paths
 * (kl_lwr_tcp_close / kl_lwr_tcp_abort / lwr_srv_err) already free the send buffer by owner. */
void  kl_lwr_send_release(void *pcb);

/* ── client (raw-API test peer, like the spike) ────────────────────────────── */

/* tcp_new + tcp_connect to ip4:port; on connect the internal callback sends `req`
 * (len bytes) and captures the response into an internal buffer. Poll it with
 * kl_lwr_client_response(). Returns 0 on the connect being issued, -1 on failure. */
int   kl_lwr_client_start(const uint8_t ip4[4], uint16_t port,
                          const void *req, size_t req_len);
/* Same, but with an explicit response-accumulator capacity (large bodies — P9-3). */
int   kl_lwr_client_start_cap(const uint8_t ip4[4], uint16_t port,
                              const void *req, size_t req_len, size_t cap);
/* Copy the client's captured response so far into `dst` (NUL-terminated if room);
 * returns the byte count copied (capped at cap-1). */
size_t kl_lwr_client_response(char *dst, size_t cap);
/* Total bytes accumulated so far (headers + body), uncapped by a copy buffer. */
size_t kl_lwr_client_len(void);
/* Body length after the CRLFCRLF header terminator (0 if not seen yet). Also reports an
 * additive checksum over the body + its first/last byte (any out ptr may be NULL) — the
 * P9-3 tests use these to verify a large/file body arrived intact. */
size_t kl_lwr_client_body(size_t *out_checksum, unsigned char *first, unsigned char *last);
/* Free the client accumulator (test teardown; avoids an at-exit leak under LSan). */
void  kl_lwr_client_release(void);
/* DEBUG: copy up to `cap` body bytes (after the CRLFCRLF) from body offset `off` into `dst`;
 * returns the count copied. Used by the P9-3 file case to pinpoint a content mismatch. */
size_t kl_lwr_client_body_peek(size_t off, unsigned char *dst, size_t cap);

/* ── P9-4 lifetime test client (separate from the P9-2/P9-3 accumulator) ───────
 * Drives the close/cancel/error cases. Each start runs ONE roundtrip; the mode controls how
 * the client tears down (exercising the server's close/err lifetime paths):
 *   mode 0 (full)          — read the whole small response, then FIN (normal short-lived conn).
 *   mode 1 (partial+RST)   — after `abort_after` received bytes, tcp_abort → RST (server tcp_err;
 *                            reset-mid-send when the server still has a large body queued).
 *   mode 2 (partial+FIN)   — after `abort_after` bytes, tcp_close → FIN (close-with-outstanding).
 * Returns 0 on the connect being issued, -1 on failure. Per-roundtrip state resets each start;
 * the completed-roundtrip counter accumulates (reset with kl_lwr_lc_reset_counter). */
int    kl_lwr_lc_start(const uint8_t ip4[4], uint16_t port, const void *req, size_t req_len,
                       int mode, size_t abort_after);
int    kl_lwr_lc_done(void);          /* this roundtrip resolved (200 seen or torn down) */
int    kl_lwr_lc_saw_200(void);       /* the response status line contained "200" */
int    kl_lwr_lc_completed(void);     /* running count of resolved roundtrips */
size_t kl_lwr_lc_recv(void);          /* bytes received this roundtrip */
void   kl_lwr_lc_reset_counter(void); /* zero the completed-roundtrip counter */

/* ── drain ─────────────────────────────────────────────────────────────────── */

/* Pull up to `max` finished records into `out`; returns the count (>= 0). */
int   kl_lwr_drain(KlLwrRecord *out, int max);

#endif /* KEEL_LWIP_RAW_GLUE_H */
