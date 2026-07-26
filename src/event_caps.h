/*
 * event_caps.h — INTERNAL. No ABI commitment (PAL Phase 7).
 *
 * The compile-time-selected event backend advertises what it can watch, so the
 * wire-up layer can negotiate it against a socket provider instead of assuming
 * they agree. Deliberately NOT in <keel/event.h>: the public event-capability
 * API is frozen only once a real completion/raw consumer lands (Phase 8 IOCP /
 * Phase 9 lwIP-raw). See docs/phase7_capability_negotiation_design.md.
 *
 * F3 (docs/pal_review.md): the socket seam and the event axis stay decoupled —
 * this header pulls only <keel/event.h>, never <keel/socket.h>. The negotiation
 * (kl_event_ctx_sockets_compatible) lives at the neutral KlEventCtx wire-up
 * (async.c), the one place that already holds both sides; it is forward-declared
 * here by opaque struct so callers need not include event_ctx to invoke it.
 */
#ifndef KEEL_SRC_EVENT_CAPS_H
#define KEEL_SRC_EVENT_CAPS_H

#include <keel/event.h>

/* Two orthogonal axes, bit flags (mirroring KL_SOCK_CAP_* style):
 *   event model  — how the loop reports work (READINESS now; COMPLETION reserved)
 *   handle domain — what handle the loop can watch (NATIVE_FD now) */
#define KL_EVENT_CAP_READINESS  (1u << 0)  /* reports read/write readiness */
#define KL_EVENT_CAP_NATIVE_FD  (1u << 1)  /* watches OS-native descriptors */
#define KL_EVENT_CAP_COMPLETION (1u << 2)  /* RESERVED — no backend yet (Phase 8) */

/* Implemented once per event backend TU (event_epoll.c, event_kqueue.c,
 * event_poll.c, event_wsapoll.c, event_iouring.c). Returns a compile-time-constant
 * set; `loop` is accepted for a future per-loop-mode backend (io_uring readiness
 * vs completion) but is unused by today's readiness backends. */
unsigned kl_event_caps(const KlEventLoop *loop);

/* Can the ctx's event loop watch the ctx's socket provider's handles? For the only
 * model shipped (readiness over native fds): the provider must expose native fds
 * AND the loop must be a native-fd readiness poller. Returns 1 if compatible, 0 if
 * not. A NULL provider is the built-in POSIX (native-fd) default. Defined in
 * async.c (the KlEventCtx wire-up); KlEventCtx is opaque here on purpose. */
struct KlEventCtx;
int kl_event_ctx_sockets_compatible(const struct KlEventCtx *ctx);

#endif /* KEEL_SRC_EVENT_CAPS_H */
