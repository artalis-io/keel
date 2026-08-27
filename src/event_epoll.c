#include <keel/event.h>
#include "event_builtin.h"
#include "event_caps.h"
#include <sys/epoll.h>
#include <unistd.h>

#define KL_EVENT_BATCH 256  /* internal stack buffer for kernel events */

/* Backend-private state: the epoll instance descriptor. Allocated through loop->alloc at init and
 * released at close; kept in loop->_backend, never on the public KlEventLoop layout. */
typedef struct {
    int fd;
} KlEpollState;

int kl_event_init_builtin(KlEventLoop *loop) {
    loop->_backend = NULL;  /* deterministic: no state until it is fully built */
    KlEpollState *st = kl_malloc(loop->alloc, sizeof(*st));
    if (!st) return -1;     /* allocator failure: nothing created, _backend stays NULL */
    /* -1 is the descriptor sentinel (epoll_create1 returns it on failure, and close() skips a
     * negative fd). The state is published only with a valid descriptor. */
    st->fd = epoll_create1(0);
    if (st->fd < 0) {
        kl_free(loop->alloc, st, sizeof(*st));  /* kernel-object failure: unwind exactly once */
        return -1;
    }
    loop->_backend = st;
    return 0;
}

static uint32_t mask_to_epoll(KlEventMask mask) {
    uint32_t ev = EPOLLET;  /* edge-triggered */
    if (mask & KL_EVENT_READ)  ev |= EPOLLIN;
    if (mask & KL_EVENT_WRITE) ev |= EPOLLOUT;
    return ev;
}

int kl_event_add_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    KlEpollState *st = loop->_backend;
    struct epoll_event ev = {
        .events = mask_to_epoll(mask),
        .data.ptr = udata,
    };
    return epoll_ctl(st->fd, EPOLL_CTL_ADD, fd, &ev);
}

int kl_event_mod_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    KlEpollState *st = loop->_backend;
    struct epoll_event ev = {
        .events = mask_to_epoll(mask),
        .data.ptr = udata,
    };
    return epoll_ctl(st->fd, EPOLL_CTL_MOD, fd, &ev);
}

int kl_event_del_builtin(KlEventLoop *loop, KlSocketHandle fd) {
    KlEpollState *st = loop->_backend;
    return epoll_ctl(st->fd, EPOLL_CTL_DEL, fd, NULL);
}

int kl_event_wait_builtin(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    KlEpollState *st = loop->_backend;
    struct epoll_event events[KL_EVENT_BATCH];
    int batch = max < KL_EVENT_BATCH ? max : KL_EVENT_BATCH;
    int n = epoll_wait(st->fd, events, batch, timeout_ms);
    if (n < 0) return -1;

    for (int i = 0; i < n; i++) {
        out[i].udata = events[i].data.ptr;
        out[i].ready = 0;
        if (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR))
            out[i].ready |= KL_EVENT_READ;
        if (events[i].events & EPOLLOUT)
            out[i].ready |= KL_EVENT_WRITE;
    }

    return n;
}

void kl_event_close_builtin(KlEventLoop *loop) {
    KlEpollState *st = loop->_backend;
    if (!st) return;                            /* idempotent */
    if (st->fd >= 0) close(st->fd);             /* close any descriptor, including 0 */
    kl_free(loop->alloc, st, sizeof(*st));
    loop->_backend = NULL;                      /* reset: resource released exactly once */
}

/* epoll is a readiness poller of native OS descriptors. */
unsigned kl_event_caps_builtin(const KlEventLoop *loop) {
    (void)loop;
    return KL_EVENT_CAP_READINESS | KL_EVENT_CAP_NATIVE_FD;
}

/* Readiness loop: the default POSIX provider works; nothing to auto-wire. */
const struct KlSocketProvider *kl_event_native_provider_builtin(const KlEventLoop *loop) {
    (void)loop;
    return NULL;
}
