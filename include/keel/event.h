#ifndef KEEL_EVENT_H
#define KEEL_EVENT_H

#include <keel/allocator.h>
#include <keel/handle.h>
#ifdef __cplusplus
extern "C" {
#endif


typedef enum {
    KL_EVENT_READ   = 1, /**< FD is readable */
    KL_EVENT_WRITE  = 2, /**< FD is writable */
    /** Interest and report: transport liveness without consuming data. As interest (alone:
     *  combined with KL_EVENT_READ or KL_EVENT_WRITE it is ignored), it asks the backend to report the peer's
     *  write-side shutdown (a FIN) or a transport failure, and never readable data. As a report,
     *  the peer shut down its sending direction or the connection failed; the socket may still be
     *  writable. A transport-level observation only: the event layer never closes, cancels or
     *  otherwise acts on it. Honored only by a loop that advertises KL_EVENT_CAP_HANGUP. A
     *  level-triggered backend reports it on every wait while the condition holds; the consumer
     *  removes or changes the registration once it has acted. */
    KL_EVENT_HANGUP = 4,
    /** Report only, never interest (ignored if requested): set with KL_EVENT_HANGUP when the
     *  backend observed a transport failure (a reset) rather than an orderly FIN. Reported only to
     *  a registration whose interest includes KL_EVENT_HANGUP; READ/WRITE registrations are
     *  reported exactly as before. Never reported without KL_EVENT_HANGUP. */
    KL_EVENT_ERROR  = 8
} KlEventMask;

typedef struct {
    void *udata;        /**< Opaque user data (connection or tagged watcher ptr) */
    KlEventMask ready;  /**< Bitmask of ready events */
} KlEvent;

/** @brief Forward declaration: the socket-provider seam (keel/socket.h). */
struct KlSocketProvider;

typedef struct KlEventOps KlEventOps;

typedef struct {
    /** Backend-owned private state, allocated through `alloc` at kl_event_init and released at
     *  kl_event_close; NULL before init and after close. Every compiled-in backend keeps its own
     *  state here (the kqueue/epoll descriptor box, the pollfd table, the io_uring ring, the IOCP
     *  completion port). Opaque to everything outside the backend TU; no public field holds a raw
     *  backend descriptor. */
    void *_backend;
    KlAllocator *alloc; /**< set before kl_event_init; used to allocate backend state */
    /** Optional runtime event backend (NULL = the compiled-in default). When set
     *  before kl_event_init, all kl_event_* calls dispatch through it; this is how
     *  a bring-your-own event backend (e.g. lwIP) is injected without recompiling
     *  the core. Install it via KlEventCtx.event_provider. */
    const KlEventOps *ops;
} KlEventLoop;

/**
 * @brief A runtime-pluggable event backend (parallel to KlSocketProvider).
 *
 * The core dispatches every kl_event_* call to these ops when a provider is
 * installed on the loop; otherwise it uses the compiled-in backend directly (no
 * indirection on the default path). A backend and its socket provider must agree
 * on what "pollable handle" means: a provider that watches non-OS handles (lwIP
 * socket indices) pairs with a socket provider whose native handles it can poll.
 *
 * Append-only vtable (see docs/contracts/compatibility.md): providers
 * zero-initialize and recompile per major version. Required (core dispatches them
 * unconditionally): init, add, mod, del, wait, close, caps. Optional:
 * native_provider (NULL = the provider offers no native socket provider) and
 * completion (NULL for a readiness backend). New ops are appended immediately
 * BEFORE the reserved `completion` slot, which MUST stay last.
 */
struct KlEventOps {
    int  (*init)(KlEventLoop *loop);
    int  (*add)(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata);
    int  (*mod)(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata);
    int  (*del)(KlEventLoop *loop, KlSocketHandle fd);
    int  (*wait)(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms);
    void (*close)(KlEventLoop *loop);
    unsigned (*caps)(const KlEventLoop *loop);
    const struct KlSocketProvider *(*native_provider)(const KlEventLoop *loop);
    /** Reserved: the backend's internal KlCompletionOps* (src/completion.h), or NULL.
     *  Opaque here: no completion type appears in this public header (cf. the opaque
     *  KlEventLoop._backend). A completion backend points this at its completion vtable
     *  so a runtime-injected provider carries the completion axis too. Advertising
     *  COMPLETION requires a non-NULL table with a drain op; installation otherwise
     *  fails and closes the initialized backend. Other operations are required only
     *  for the features the provider supports. Readiness
     *  backends leave it NULL. MUST stay the last member; no ABI shuffle. */
    const void *completion;
};

/* Event-backend capability bits returned by KlEventOps.caps(): the provider
 * describes how it reports work and what handles it can watch, so the wire-up can
 * check it against a socket provider. Two orthogonal axes:
 *   model:   KL_EVENT_CAP_READINESS (report readiness) / _COMPLETION (deliver
 *            completions; built-in or runtime-supplied via the completion vtable)
 *   handle:  KL_EVENT_CAP_NATIVE_FD (watches OS descriptors) */
#define KL_EVENT_CAP_READINESS  (1u << 0)
#define KL_EVENT_CAP_NATIVE_FD  (1u << 1)
#define KL_EVENT_CAP_COMPLETION (1u << 2)
/* KL_EVENT_HANGUP is honored, as a guarantee of this backend's implementation (not of the OS):
 * with HANGUP interest alone it reports a FIN, including one behind unread bytes;
 * reports a reset with KL_EVENT_ERROR; never reports readable data; and never consumes data.
 * A backend sets it only where its tests prove all four. Watchers (kl_watcher_add) may use HANGUP
 * interest on such a loop, readiness or completion. */
#define KL_EVENT_CAP_HANGUP     (1u << 3)

/** @brief A named event-backend provider handed to KlEventCtx.event_provider. */
typedef struct KlEventProvider {
    const KlEventOps *ops;
    const char       *name;
} KlEventProvider;

/** @brief Initialize the compiled-in platform event loop (epoll/kqueue/io_uring/
 *  poll/…). Sets loop->ops = NULL (the builtin path). */
int  kl_event_init(KlEventLoop *loop);

/** @brief Initialize the loop on a runtime-supplied event backend instead of the
 *  compiled-in one. Installs provider->ops on the loop and runs its init. A NULL
 *  provider falls back to kl_event_init(). Used by KlEventCtx when an
 *  event_provider is configured (e.g. lwIP). */
int  kl_event_init_provider(KlEventLoop *loop, const KlEventProvider *provider);

/** @brief Register a file descriptor for events. */
int  kl_event_add(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata);

/** @brief Modify the event mask for a registered fd. */
int  kl_event_mod(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata);

/**
 * @brief Remove a file descriptor from the event loop.
 *
 * Remove a handle BEFORE closing it. Keel's registration guarantees hold only for that lifecycle
 * (register, del, then close); no backend protects a registration against its handle being closed,
 * duplicated or reused behind the loop's back. A handle closed while still registered leaves a
 * stale registration, and the OS may give its handle value to an unrelated socket. That breaks
 * this contract, and what follows is damage limitation per backend, not a guarantee. Registrations
 * are named by handle VALUE: once the value is reused and registered again, the old owner's
 * kl_event_del / kl_event_mod / kl_event_add acts on the new owner's registration.
 *   - epoll: the registration belongs to the open file, not the descriptor; the kernel drops it
 *     only when the last descriptor for that file closes. While another one keeps the file open
 *     (dup, fork, descriptor passing) it goes on reporting to its old owner, and kl_event_del by
 *     the closed number can no longer remove it.
 *   - kqueue: the kernel drops the registration with the descriptor.
 *   - poll, pollcomp (watches): the registration stays. Until the descriptor number is reused,
 *     poll() flags it invalid (POLLNVAL) and returns at once, so a wait does not block for its
 *     timeout and the loop spins; poll reports no event for it, pollcomp delivers an event with an
 *     empty ready mask to the stale watcher on every drain. Once POSIX reuses the number (it
 *     reuses the lowest free one at once), the stale registration reports the new descriptor's
 *     readiness to its old owner.
 *   - io_uring (watches): an outstanding poll holds its own reference to the closed file, so what
 *     it reports afterwards is unspecified; a later re-arm by the owner polls whatever the number
 *     names by then.
 *   - Windows select(): the registration stays and is polled by handle value. Only a wait whose
 *     select() fails with WSAENOTSOCK detects it; from then on it is not reported until a
 *     kl_event_add or kl_event_mod names the value. If Winsock gives the value to another socket
 *     before such a wait (it commonly reuses a closed handle's value at once), select() accepts it
 *     and the stale registration reports the new socket's readiness to its old owner, as on poll.
 *     A wait whose select() still fails after that check returns -1. kl_event_del or
 *     kl_event_close releases the registration.
 *   - IOCP (watches): never reported again and no I/O is posted on the handle, until a
 *     registration (kl_event_add / kl_event_mod) names it again AND the handle is verifiably on
 *     this loop's port: freshly associated by that call, or recorded as associated by this loop
 *     (a record can outlive a socket closed without telling the loop). A socket owned by another
 *     loop is otherwise refused (the call fails). kl_event_del or kl_event_close still releases
 *     the stale registration. Not covered: a close and reuse of the value during a watch's IDLE
 *     period (from a completion being dequeued to the probe being posted again: inside the
 *     watcher's own callback, in an earlier callback of the same batch, between ticks for a watch
 *     whose socket is in error, or before a retried post). The repeat post in that period (the
 *     re-arm of the unchanged registration by kl_watcher_rearm, which the dispatch calls after
 *     the callback, and the retry) is not re-checked and may land on the new socket, completing on
 *     no port or on another loop's; any other registration in that period (kl_watcher_add or
 *     kl_watcher_mod, even with the same interest) is checked. kl_event_close then gives up on such
 *     probes (after about 2 s without progress, and only if nothing but such probes is
 *     outstanding) and retains, never frees, their small operation records (no caller memory is
 *     involved); another loop that dequeues one recognises it as foreign and drops it untouched.
 */
int  kl_event_del(KlEventLoop *loop, KlSocketHandle fd);

/**
 * @brief Wait for events.
 * @param loop       Event loop instance.
 * @param out        Array to receive ready events.
 * @param max        Maximum events to return.
 * @param timeout_ms Timeout in milliseconds (-1 for infinite).
 * @return Number of ready events, or -1 on error. A wait a signal interrupts returns 0, as a
 *         timeout does, so a caller looping while the result is >= 0 keeps running.
 */
int  kl_event_wait(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms);

/** @brief Close and clean up the event loop. */
void kl_event_close(KlEventLoop *loop);

#ifdef __cplusplus
}
#endif

#endif
