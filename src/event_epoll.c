#include <keel/event.h>
#include "event_builtin.h"
#include "event_caps.h"
#include <sys/epoll.h>
#include <unistd.h>

#define KL_EVENT_BATCH 256  /* internal stack buffer for kernel events */

int kl_event_init_builtin(KlEventLoop *loop) {
    loop->fd = epoll_create1(0);
    return loop->fd < 0 ? -1 : 0;
}

static uint32_t mask_to_epoll(KlEventMask mask) {
    uint32_t ev = EPOLLET;  /* edge-triggered */
    if (mask & KL_EVENT_READ)  ev |= EPOLLIN;
    if (mask & KL_EVENT_WRITE) ev |= EPOLLOUT;
    return ev;
}

int kl_event_add_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    struct epoll_event ev = {
        .events = mask_to_epoll(mask),
        .data.ptr = udata,
    };
    return epoll_ctl(loop->fd, EPOLL_CTL_ADD, fd, &ev);
}

int kl_event_mod_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    struct epoll_event ev = {
        .events = mask_to_epoll(mask),
        .data.ptr = udata,
    };
    return epoll_ctl(loop->fd, EPOLL_CTL_MOD, fd, &ev);
}

int kl_event_del_builtin(KlEventLoop *loop, KlSocketHandle fd) {
    return epoll_ctl(loop->fd, EPOLL_CTL_DEL, fd, NULL);
}

int kl_event_wait_builtin(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    struct epoll_event events[KL_EVENT_BATCH];
    int batch = max < KL_EVENT_BATCH ? max : KL_EVENT_BATCH;
    int n = epoll_wait(loop->fd, events, batch, timeout_ms);
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
    if (loop->fd >= 0) {
        close(loop->fd);
        loop->fd = -1;
    }
}

/* epoll is a readiness poller of native OS descriptors. */
unsigned kl_event_caps_builtin(const KlEventLoop *loop) {
    (void)loop;
    return KL_EVENT_CAP_READINESS | KL_EVENT_CAP_NATIVE_FD;
}

/* Readiness loop — the default POSIX provider works; nothing to auto-wire. */
const struct KlSocketProvider *kl_event_native_provider_builtin(const KlEventLoop *loop) {
    (void)loop;
    return NULL;
}
