#include <keel/event.h>
#include "event_caps.h"
#include <sys/event.h>
#include <unistd.h>

#define KL_EVENT_BATCH 256  /* internal stack buffer for kernel events */

int kl_event_init(KlEventLoop *loop) {
    loop->fd = kqueue();
    return loop->fd < 0 ? -1 : 0;
}

int kl_event_add(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    struct kevent changes[2];
    int n = 0;

    if (mask & KL_EVENT_READ) {
        EV_SET(&changes[n], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, udata);
        n++;
    }
    if (mask & KL_EVENT_WRITE) {
        EV_SET(&changes[n], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, udata);
        n++;
    }

    return kevent(loop->fd, changes, n, NULL, 0, NULL) < 0 ? -1 : 0;
}

int kl_event_mod(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    /*
     * kqueue: EV_ADD on an existing filter updates it in place (upsert).
     * EV_ADD | EV_ENABLE re-enables a disabled filter.
     * We add the desired filters and disable the unwanted ones.
     *
     * Both READ and WRITE are always registered (via initial add + first mod),
     * so subsequent mods just enable/disable. The mask may request READ,
     * WRITE, or both (e.g., HTTP/2 connections need simultaneous read+write).
     */
    struct kevent changes[2];

    unsigned short read_flags = (mask & KL_EVENT_READ)
        ? (EV_ADD | EV_ENABLE | EV_CLEAR)
        : (EV_ADD | EV_DISABLE);
    unsigned short write_flags = (mask & KL_EVENT_WRITE)
        ? (EV_ADD | EV_ENABLE | EV_CLEAR)
        : (EV_ADD | EV_DISABLE);

    EV_SET(&changes[0], fd, EVFILT_READ, read_flags, 0, 0, udata);
    EV_SET(&changes[1], fd, EVFILT_WRITE, write_flags, 0, 0, udata);

    return kevent(loop->fd, changes, 2, NULL, 0, NULL) < 0 ? -1 : 0;
}

int kl_event_del(KlEventLoop *loop, KlSocketHandle fd) {
    struct kevent changes[2];
    EV_SET(&changes[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    return kevent(loop->fd, changes, 2, NULL, 0, NULL) < 0 ? -1 : 0;
}

int kl_event_wait(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    struct kevent events[KL_EVENT_BATCH];
    int batch = max < KL_EVENT_BATCH ? max : KL_EVENT_BATCH;
    struct timespec ts;
    struct timespec *tsp = NULL;

    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }

    int n = kevent(loop->fd, NULL, 0, events, batch, tsp);
    if (n < 0) return -1;

    for (int i = 0; i < n; i++) {
        out[i].udata = events[i].udata;
        out[i].ready = 0;
        if (events[i].filter == EVFILT_READ)
            out[i].ready |= KL_EVENT_READ;
        if (events[i].filter == EVFILT_WRITE)
            out[i].ready |= KL_EVENT_WRITE;
    }

    return n;
}

void kl_event_close(KlEventLoop *loop) {
    if (loop->fd >= 0) {
        close(loop->fd);
        loop->fd = -1;
    }
}

/* PAL Phase 7: kqueue is a readiness poller of native OS descriptors. */
unsigned kl_event_caps(const KlEventLoop *loop) {
    (void)loop;
    return KL_EVENT_CAP_READINESS | KL_EVENT_CAP_NATIVE_FD;
}
