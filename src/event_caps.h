/*
 * event_caps.h: INTERNAL. No ABI commitment.
 *
 * The compile-time-selected event backend advertises what it can watch, so the
 * wire-up layer can negotiate it against a socket provider instead of assuming
 * they agree. Deliberately NOT in <keel/event.h>: the public event-capability
 * API stays internal. See docs/archive/phases/phase7_capability_negotiation_design.md.
 *
 * The socket seam and the event axis stay decoupled (docs/archive/audits/pal_review.md):
 * this header pulls only <keel/event.h>, never <keel/socket.h>. The negotiation
 * (kl_event_ctx_sockets_compatible) lives at the neutral KlEventCtx wire-up
 * (async.c), the one place that already holds both sides; it is forward-declared
 * here by opaque struct so callers need not include event_ctx to invoke it.
 */
#ifndef KEEL_SRC_EVENT_CAPS_H
#define KEEL_SRC_EVENT_CAPS_H

#include <keel/event.h>

/* The KL_EVENT_CAP_* bits are now public (keel/event.h): part of the
 * KlEventOps.caps() provider contract. This header keeps only the negotiation
 * helpers below. */

/* Implemented once per event backend TU (event_epoll.c, event_kqueue.c,
 * event_poll.c, event_wsapoll.c, event_iouring.c). Returns a compile-time-constant
 * set; `loop` is accepted for a future per-loop-mode backend (io_uring readiness
 * vs completion) but is unused by today's readiness backends. */
unsigned kl_event_caps(const KlEventLoop *loop);

/* The socket provider this loop needs but the default POSIX provider can't satisfy, or NULL
 * if the loop runs on the default (readiness) provider. A completion backend
 * returns its overlapped provider (kl_socket_provider_iocp/pollcomp/iouring); every
 * readiness backend returns NULL. Lets the server/client auto-wire the matching provider on a
 * completion loop when the caller configured none (or an incompatible one), so a completion
 * backend is a source-compatible drop-in; the event axis stays masked above the build flag.
 * Implemented once per event backend TU (like kl_event_caps); the return type is the opaque
 * provider (event_caps.h still pulls no socket-seam header). */
const struct KlSocketProvider *kl_event_native_provider(const KlEventLoop *loop);

/* Pure negotiation over the two axes: takes the event-loop caps explicitly (so it
 * is unit-testable without a real backend). Returns 1 if the loop can drive the
 * provider, 0 if not. A NULL provider is the built-in POSIX (native-fd) default.
 *   - completion loop (KL_EVENT_CAP_COMPLETION): the provider must route I/O through
 *     the loop's overlapped submit path (KL_SOCK_CAP_OVERLAPPED).
 *   - readiness loop: the provider must expose native fds AND the loop must be a
 *     native-fd readiness poller.
 * Defined in async.c; KlSocketProvider is opaque here (no socket-seam include). */
struct KlSocketProvider;
int kl_caps_compatible(unsigned ev_caps, const struct KlSocketProvider *sockets);

/* Can the ctx's event loop watch/drive the ctx's socket provider? Convenience wrapper
 * over kl_caps_compatible with the ctx's actual backend caps. Defined in async.c;
 * KlEventCtx is opaque here on purpose. */
struct KlEventCtx;
int kl_event_ctx_sockets_compatible(const struct KlEventCtx *ctx);

#endif /* KEEL_SRC_EVENT_CAPS_H */
