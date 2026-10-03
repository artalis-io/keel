#ifndef KEEL_SRC_COMPLETION_INTERNAL_H
#define KEEL_SRC_COMPLETION_INTERNAL_H

/*
 * completion_internal.h: INTERNAL. The cross-TU surface shared by the split completion driver.
 *
 * The completion driver is split so that referencing the generic tick does not pull the
 * whole server + UDP stack. The split separates:
 *
 *   completion_core.c:   the generic tick (kl_comp_run). Routes ACCEPT/READ/WRITE and
 *                         UDP kinds through the two opaque KlEventCtx hooks
 *                         (comp_conn_dispatch / comp_udp_dispatch) so a client-only
 *                         build (CONNECT/WATCHER + timers only) links neither the
 *                         server nor UDP handlers.
 *   completion_http_server.c: the KlHttpConn/HTTP-1 server state machine over completions,
 *                         PLUS the server-side memory-BIO TLS leg (comp_tls_*). TLS is
 *                         folded in because comp_tls_drive ↔ comp_after_state ↔
 *                         comp_h2_drive mutually recurse; separating TLS would need a
 *                         forward-declaration web with no real decoupling benefit.
 *                         Registers comp_server_conn_dispatch on the ctx hook.
 *   completion_http2.c:     comp_h2_drive (HTTP/2 over completions).
 *   completion_ws.c:     comp_ws_drive (WebSocket over completions).
 *
 * This header declares the handful of helpers that now cross those TU boundaries.
 * The shared surface is deliberately minimal:
 *   - server → h2/ws:  comp_h2_drive / comp_ws_drive
 *   - h2/ws → server:  comp_close, comp_tls_flush, comp_tls_drain_output
 * INTERNAL header: not installed, no ABI commitment.
 */

#include <keel/http_connection.h>   /* KlHttpConn */
#include <stddef.h>            /* size_t */

struct KlHttpServer;

/* ── server-side helpers the h2/ws drive TUs call back into ──────────────── */

/* Release the connection (close the socket + return the pool slot). */
void kl_comp_close(struct KlHttpServer *s, KlHttpConn *c);

/* Queue the TLS engine's pending outgoing ciphertext on the connection's output queue and start
 * sending it (one overlapped send at a time, in order). Never blocks. 0, or -1 on error. */
int  kl_comp_tls_flush(KlHttpConn *c);

/* Close once the connection's queued TLS output is out (or now, when nothing is queued). */
void kl_comp_close_after_output(struct KlHttpServer *s, KlHttpConn *c);

/* Post the next recv once the queued TLS output is out (or now, when nothing is queued). */
void kl_comp_recv_after_output(struct KlHttpServer *s, KlHttpConn *c);

/* ── the h2/ws drive functions the server dispatch calls ─────────────────── */

/* Drive an established HTTP/2 connection over the completion loop. */
void kl_comp_http2_drive(struct KlHttpServer *s, KlHttpConn *c);

/* Drive an established WebSocket connection over the completion loop. */
void kl_comp_ws_drive(struct KlHttpServer *s, KlHttpConn *c);

#endif /* KEEL_SRC_COMPLETION_INTERNAL_H */
