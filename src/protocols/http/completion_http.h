#ifndef KEEL_PROTOCOLS_HTTP_COMPLETION_HTTP_H
#define KEEL_PROTOCOLS_HTTP_COMPLETION_HTTP_H

/*
 * completion_http.h: the HTTP leg of the completion axis. Owned by src/protocols/http/.
 *
 * The substrate completion axis (src/completion.h) is protocol-neutral: its vtable +
 * `kl_comp_*_raw` entry points speak only KlEventCtx / KlStream / KlSocketHandle. This
 * header is the HTTP adapter over it, in two tiers (docs/archive/freezes/protocols_restructure_freeze.md §4.8):
 *
 *   - `kl_comp_*` (KlHttpConn / KlHttpServer form): thin wrappers that each mirror a neutral
 *     `kl_comp_*_raw` operation 1:1, extracting the neutral arg (&s->ev, s->listen_fd, &c->stream)
 *     and forwarding. Defined in completion_http_server.c (hosted) / completion_http_absent.c
 *     (KEEL_NO_COMPLETION). Consumed by the HTTP-1/h2/ws completion adapters.
 *   - `kl_http_comp_*`: HTTP-only completion orchestration with NO neutral counterpart (the run
 *     loop's completion tick, accept quiescence, async resume, body re-post). Consumed by http_server_core.c (run/quiesce/post_read) + async.c (resume).
 *
 * The neutral run-loop tick kl_comp_run() and the neutral cancel/datagram/connect seam stay in
 * src/completion_io.h; TUs that need only those keep including it.
 */

#include "completion.h"              /* neutral axis: KlIoVec, KlStream, KlCompletionEvent, _raw */
#include <keel/http_connection.h>    /* KlHttpConn */
#include <stddef.h>                  /* size_t */
#include <stdint.h>                  /* uint64_t */

struct KlHttpServer;

/* ── Tier 2: HTTP wrappers mirroring a neutral kl_comp_*_raw op 1:1 ─────────────────── */

/* Choose the receive buffer for the connection's current phase: plaintext/PROXY →
 * c->stream.read_buf; TLS → the per-conn ciphertext scratch (c->comp_cipher); then post the raw
 * receive (kl_comp_post_recv_raw). This is where all TLS/PROXY/state knowledge lives; the backend
 * sees only (stream, buf, cap). */
int kl_comp_post_recv(KlHttpConn *c);

/* Post the (already-serialized) response iovec via kl_comp_post_send_raw. See the RESPONSE-SEGMENT
 * LIFETIME CONTRACT on kl_comp_post_send_raw (completion.h). */
int kl_comp_post_send(KlHttpConn *c, const KlIoVec *iov, int iovcnt, size_t total);

/* Prime the server's accept backlog (kl_comp_prime_accepts_raw with &s->ev + s->listen_fd). One-time
 * setup that RETURNS the backend's accept window (>=1 post-driven, 0 autonomous, <0 fatal). */
int kl_comp_prime_accepts(struct KlHttpServer *s);

/* Post one async accept on the server's listen socket (kl_comp_post_accept_raw with &s->ev). */
int kl_comp_post_accept(struct KlHttpServer *s);

/* Teardown accept-side force-completion (kl_comp_shutdown_accepts_raw with &s->ev). Returns 0 (incl. a
 * NULL/autonomous/readiness no-op), -1 if the force could not be guaranteed. */
int kl_comp_shutdown_accepts(struct KlHttpServer *s);

/* Post one async file send: the serialized response head then `count` bytes from `file_fd`
 * (kl_comp_post_sendfile_raw with &c->stream). Reports a KL_COMP_WRITE on completion. */
int kl_comp_post_sendfile(KlHttpConn *c, const KlIoVec *head_iov, int head_n,
                          size_t head_total, int file_fd, uint64_t count);

/* ── Tier 3: HTTP-only completion orchestration (no neutral counterpart) ────────────── */

/* Run one completion-loop tick for the server: install the conn-dispatch hook, set up the accept
 * path once from the backend's window, then drive one generic tick (kl_comp_run) over its event ctx.
 * Returns 0 to continue the run loop, -1 to stop. */
int kl_http_comp_run(struct KlHttpServer *server, int timeout_ms);

/* Teardown accept quiescence: force every posted accept to completion, then reap to confirmed
 * KlListener detachment with a teardown-specific dispatcher (no live tick). Returns 0, or -1 if the
 * force could not be guaranteed. Call once, after the listen socket is closed. */
int kl_http_comp_quiesce_accepts(struct KlHttpServer *server);

/* Resume a suspended connection on a completion loop after an async op completed: drive the
 * completion send path for the state on_resume produced. */
void kl_http_comp_resume(struct KlHttpServer *server, KlHttpConn *conn);

/* Re-arm a body read on a completion loop after read-side flow control resumes: post a fresh recv.
 * */
void kl_http_comp_post_read(KlHttpConn *conn);

#endif /* KEEL_PROTOCOLS_HTTP_COMPLETION_HTTP_H */
