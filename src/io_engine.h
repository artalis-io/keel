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

struct KlServer;
struct KlEventCtx;
struct KlUdp;

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

/* Post one overlapped datagram receive for a UDP socket on a completion loop (into
 * udp->recv_buf). Called by udp.c (shared) on a completion loop, so — like
 * kl_comp_run — it is declared here and stubbed in io_engine.c on non-completion
 * builds (never reached there); the real primitive lives in event_iocp.c. */
int kl_comp_post_udp_recv(struct KlUdp *udp);

#endif /* KEEL_SRC_IO_ENGINE_H */
