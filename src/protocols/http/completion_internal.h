#ifndef KEEL_SRC_COMPLETION_INTERNAL_H
#define KEEL_SRC_COMPLETION_INTERNAL_H

/*
 * completion_internal.h — INTERNAL. The cross-TU surface shared by the split
 * completion driver (freestanding step B2a).
 *
 * completion_driver.c was one 800-line TU bundling the generic completion tick with
 * the server/TLS/H2/WS connection processing, so referencing the generic tick pulled
 * the whole server + UDP stack. The split separates:
 *
 *   completion_core.c   — the generic tick (kl_comp_run). Routes ACCEPT/READ/WRITE and
 *                         UDP kinds through the two opaque KlEventCtx hooks
 *                         (comp_conn_dispatch / comp_udp_dispatch) so a client-only
 *                         build (CONNECT/WATCHER + timers only) links neither the
 *                         server nor UDP handlers.
 *   completion_http_server.c — the KlHttpConn/HTTP-1 server state machine over completions,
 *                         PLUS the server-side memory-BIO TLS leg (comp_tls_*). TLS is
 *                         folded in because comp_tls_drive ↔ comp_after_state ↔
 *                         comp_h2_drive mutually recurse — separating TLS would need a
 *                         forward-declaration web with no real decoupling benefit.
 *                         Registers comp_server_conn_dispatch on the ctx hook.
 *   completion_http2.c     — comp_h2_drive (HTTP/2 over completions).
 *   completion_ws.c     — comp_ws_drive (WebSocket over completions).
 *
 * This header declares the handful of helpers that now cross those TU boundaries.
 * The shared surface is deliberately minimal:
 *   - server → h2/ws:  comp_h2_drive / comp_ws_drive
 *   - h2/ws → server:  comp_close, comp_tls_flush, comp_tls_drain_output
 * INTERNAL header — not installed, no ABI commitment.
 */

#include <keel/http_connection.h>   /* KlHttpConn */
#include <stddef.h>            /* size_t */

struct KlHttpServer;

/* ── server-side helpers the h2/ws drive TUs call back into ──────────────── */

/* Release the connection (close the socket + return the pool slot). */
void kl_comp_close(struct KlHttpServer *s, KlHttpConn *c);

/* Push the TLS engine's pending outgoing ciphertext to the socket synchronously
 * (memory-BIO out ring → socket). Returns 0 on success, -1 on a fatal send error. */
int  kl_comp_tls_flush(KlHttpConn *c);

/* Drain the TLS engine's pending outgoing ciphertext into one heap buffer (grow as it
 * fills). On success returns 0 with out/outlen/outcap set (caller frees outcap bytes),
 * -1 on error. Used to post h2 output overlapped instead of a synchronous flush. */
int  kl_comp_tls_drain_output(KlHttpConn *c, unsigned char **out, size_t *outlen,
                              size_t *outcap);

/* ── the h2/ws drive functions the server dispatch calls ─────────────────── */

/* Drive an established HTTP/2 connection over the completion loop. */
void kl_comp_http2_drive(struct KlHttpServer *s, KlHttpConn *c);

/* Drive an established WebSocket connection over the completion loop. */
void kl_comp_ws_drive(struct KlHttpServer *s, KlHttpConn *c);

#endif /* KEEL_SRC_COMPLETION_INTERNAL_H */
