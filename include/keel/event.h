#ifndef KEEL_EVENT_H
#define KEEL_EVENT_H

#include <keel/allocator.h>
#include <keel/handle.h>
#ifdef __cplusplus
extern "C" {
#endif


typedef enum {
    KL_EVENT_READ  = 1, /**< FD is readable */
    KL_EVENT_WRITE = 2  /**< FD is writable */
} KlEventMask;

typedef struct {
    void *udata;        /**< Opaque user data (connection or tagged watcher ptr) */
    KlEventMask ready;  /**< Bitmask of ready events */
} KlEvent;

/** @brief Forward declaration: the socket-provider seam (keel/socket.h). */
struct KlSocketProvider;

typedef struct KlEventOps KlEventOps;

typedef struct {
    int fd;             /**< epoll_fd or kqueue_fd, -1 for io_uring */
    void *_backend;     /**< reserved for backend-specific state */
    KlAllocator *alloc; /**< set before kl_event_init; used by io_uring backend */
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
     *  so a runtime-injected provider carries the completion axis too; readiness
     *  backends leave it NULL. MUST stay the last member; no ABI shuffle. */
    const void *completion;
};

/* Event-backend capability bits returned by KlEventOps.caps(): the provider
 * describes how it reports work and what handles it can watch, so the wire-up can
 * check it against a socket provider. Two orthogonal axes:
 *   model:   KL_EVENT_CAP_READINESS (report readiness) / _COMPLETION (deliver
 *            completions; build-time backends only, not runtime-injectable today)
 *   handle:  KL_EVENT_CAP_NATIVE_FD (watches OS descriptors) */
#define KL_EVENT_CAP_READINESS  (1u << 0)
#define KL_EVENT_CAP_NATIVE_FD  (1u << 1)
#define KL_EVENT_CAP_COMPLETION (1u << 2)

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

/** @brief Remove a file descriptor from the event loop. */
int  kl_event_del(KlEventLoop *loop, KlSocketHandle fd);

/**
 * @brief Wait for events.
 * @param loop       Event loop instance.
 * @param out        Array to receive ready events.
 * @param max        Maximum events to return.
 * @param timeout_ms Timeout in milliseconds (-1 for infinite).
 * @return Number of ready events, or -1 on error.
 */
int  kl_event_wait(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms);

/** @brief Close and clean up the event loop. */
void kl_event_close(KlEventLoop *loop);

#ifdef __cplusplus
}
#endif

#endif
