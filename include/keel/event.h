#ifndef KEEL_EVENT_H
#define KEEL_EVENT_H

#include <keel/allocator.h>

typedef enum {
    KL_EVENT_READ  = 1, /**< FD is readable */
    KL_EVENT_WRITE = 2  /**< FD is writable */
} KlEventMask;

typedef struct {
    void *udata;        /**< Opaque user data (connection or tagged watcher ptr) */
    KlEventMask ready;  /**< Bitmask of ready events */
} KlEvent;

typedef struct {
    int fd;             /**< epoll_fd or kqueue_fd, -1 for io_uring */
    void *_backend;     /**< reserved for backend-specific state */
    KlAllocator *alloc; /**< set before kl_event_init; used by io_uring backend */
} KlEventLoop;

/** @brief Initialize the platform event loop (epoll/kqueue/io_uring). */
int  kl_event_init(KlEventLoop *loop);

/** @brief Register a file descriptor for events. */
int  kl_event_add(KlEventLoop *loop, int fd, KlEventMask mask, void *udata);

/** @brief Modify the event mask for a registered fd. */
int  kl_event_mod(KlEventLoop *loop, int fd, KlEventMask mask, void *udata);

/** @brief Remove a file descriptor from the event loop. */
int  kl_event_del(KlEventLoop *loop, int fd);

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

#endif
