/*
 * io_engine.h — INTERNAL. The event-model dispatch seam (PAL Phase 8).
 *
 * The server run loop is readiness-shaped inline (kl_event_wait → dispatch →
 * conn_read/write). When the event loop is a *completion* loop (IOCP, advertising
 * KL_EVENT_CAP_COMPLETION), the server delegates one tick here instead — so the
 * completion model stays contained to the backend and never leaks into the shared
 * readiness path or Keel's public API.
 *
 * Defined per-backend, selected by the Makefile (no #ifdef in shared code):
 *   - readiness builds link the stub in io_engine.c (never called — the server only
 *     enters the completion branch when the loop advertises COMPLETION);
 *   - the IOCP build links the real tick in event_iocp.c.
 * KlServer is used by-pointer only, so an opaque forward declaration keeps this
 * header free of any other dependency. See docs/phase8_iocp_design.md.
 */
#ifndef KEEL_SRC_IO_ENGINE_H
#define KEEL_SRC_IO_ENGINE_H

#include <stddef.h>   /* size_t (kl_comp_post_udp_send) */
#include <keel/handle.h>   /* KlSocketHandle (kl_comp_cancel) */

struct KlServer;
struct KlEventCtx;
struct KlUdp;
struct sockaddr;

/* Run one completion-loop tick for the server: prime accepts, then drive one
 * generic tick over its event ctx. Returns 0 to continue the run loop, <0 to stop. */
int kl_io_engine_run_completion(struct KlServer *s, int timeout_ms);

/* The generic completion tick: drain the ctx's completion loop and route each op to
 * its consumer (connections; datagrams in 8b-4c). Shared by the server run loop and
 * the standalone kl_event_ctx_run, so standalone consumers (UDP/DNS) run on a
 * completion loop. Defined per-backend, selected by the Makefile (no #ifdef in shared
 * code): the completion driver (completion_driver.c) provides the real tick; the
 * io_engine.c stub is linked on readiness builds and never called (no readiness
 * backend advertises COMPLETION). Returns events processed (>= 0), or -1. */
int kl_comp_run(struct KlEventCtx *ctx, int max, int timeout_ms);

/* Cancel any pending completion ops on `fd` (PAL hardening — completion-path idle
 * timeout). On a completion loop a conn waiting on an overlapped read holds that op
 * indefinitely; the server's idle-timeout sweep calls this to abort it. The op then
 * completes with an error, and the driver releases the connection through its normal
 * completion path (so there is never a dangling op or a double release). Backend-defined
 * (event_pollcomp.c removes the ops; event_iocp.c CancelIoEx's them); stubbed in
 * io_engine.c on readiness builds, where it is never called. */
void kl_comp_cancel(struct KlEventCtx *ctx, KlSocketHandle fd);

/* Post one overlapped datagram receive for a UDP socket on a completion loop (into
 * udp->recv_buf). Called by udp.c (shared) on a completion loop, so — like
 * kl_comp_run — it is declared here and stubbed in io_engine.c on non-completion
 * builds (never reached there); the real primitive lives in event_iocp.c. */
int kl_comp_post_udp_recv(struct KlUdp *udp);

/* Post one overlapped datagram send (WSASendTo) on a completion loop. The backend
 * copies the datagram + destination for the op's lifetime; the completion surfaces
 * a KL_COMP_UDP_SEND event. Shared-called by udp.c → stubbed in io_engine.c on
 * non-completion builds; real primitive in event_iocp.c. */
int kl_comp_post_udp_send(struct KlUdp *udp, const void *data, size_t len,
                          const struct sockaddr *dest, int dest_len);

#endif /* KEEL_SRC_IO_ENGINE_H */
