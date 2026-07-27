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

/* Run one completion-loop tick: drain finished overlapped ops and drive their
 * connections. Returns 0 to continue the run loop, <0 to stop it. */
int kl_io_engine_run_completion(struct KlServer *s, int timeout_ms);

#endif /* KEEL_SRC_IO_ENGINE_H */
