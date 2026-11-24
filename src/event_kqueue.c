#include <keel/event.h>
#include "event_builtin.h"
#include "event_caps.h"
#include "event_hangup.h"
#include <sys/event.h>
#include <fcntl.h>       /* FD_CLOEXEC on the kqueue descriptor */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define KL_EVENT_BATCH 256  /* internal stack buffer for kernel events */

/* Backend-private state: the kqueue instance descriptor. Allocated through loop->alloc at init and
 * released at close; kept in loop->_backend, never on the public KlEventLoop layout. */
typedef struct {
    int fd;
    /* Which descriptors are hang-up registrations (KL_EVENT_HANGUP interest alone): indexed by fd,
     * grown on demand. Decided by the interest registered, never by the caller's udata. */
    unsigned char *hup;
    size_t hup_cap;
} KlKqueueState;

/* Record whether `fd` is a hang-up registration. Clearing never allocates. -1 on allocation failure. */
static int kq_set_hup(KlEventLoop *loop, KlKqueueState *st, KlSocketHandle fd, int on) {
    if (fd < 0) return -1;
    size_t i = (size_t)fd;
    if (i >= st->hup_cap) {
        if (!on) return 0;
        size_t cap = st->hup_cap ? st->hup_cap : 64;
        while (cap <= i) {
            if (cap > SIZE_MAX / 2) return -1;
            cap *= 2;
        }
        unsigned char *n = kl_realloc(loop->alloc, st->hup, st->hup_cap, cap);
        if (!n) return -1;
        memset(n + st->hup_cap, 0, cap - st->hup_cap);
        st->hup = n;
        st->hup_cap = cap;
    }
    st->hup[i] = on ? 1 : 0;
    return 0;
}

static int kq_is_hup(const KlKqueueState *st, uintptr_t ident) {
    return ident < st->hup_cap && st->hup[ident];
}

int kl_event_init_builtin(KlEventLoop *loop) {
    loop->_backend = NULL;  /* deterministic: no state until it is fully built */
    KlKqueueState *st = kl_malloc(loop->alloc, sizeof(*st));
    if (!st) return -1;     /* allocator failure: nothing created, _backend stays NULL */
    /* -1 is the descriptor sentinel (kqueue returns it on failure, and close() skips a negative
     * fd). The state is published only with a valid descriptor. */
    st->hup = NULL;
    st->hup_cap = 0;
    st->fd = kqueue();
    if (st->fd < 0) {
        kl_free(loop->alloc, st, sizeof(*st));  /* kernel-object failure: unwind exactly once */
        return -1;
    }
    /* A kqueue is not inherited across fork(), but mark it close-on-exec anyway so the invariant has
     * no exceptions: every descriptor Keel creates is close-on-exec. */
    int fdf = fcntl(st->fd, F_GETFD, 0);
    if (fdf >= 0) (void)fcntl(st->fd, F_SETFD, fdf | FD_CLOEXEC);
    loop->_backend = st;
    return 0;
}

int kl_event_add_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    KlKqueueState *st = loop->_backend;
    struct kevent changes[2];
    int n = 0;

    /* A hang-up registration watches EVFILT_READ and is recorded in st->hup: the wait reports only
     * the EV_EOF it carries (the FIN or a reset) and swallows a report for data alone. */
    int hup = kl_hup_wanted(mask);
    if (kq_set_hup(loop, st, fd, hup) < 0) return -1;
    if ((mask & KL_EVENT_READ) || hup) {
        EV_SET(&changes[n], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, udata);
        n++;
    }
    if (mask & KL_EVENT_WRITE) {
        EV_SET(&changes[n], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, udata);
        n++;
    }

    return kevent(st->fd, changes, n, NULL, 0, NULL) < 0 ? -1 : 0;
}

int kl_event_mod_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    /*
     * kqueue: EV_ADD on an existing filter updates it in place (upsert).
     * EV_ADD | EV_ENABLE re-enables a disabled filter.
     * We add the desired filters and disable the unwanted ones.
     *
     * Both READ and WRITE are always registered (via initial add + first mod),
     * so subsequent mods just enable/disable. The mask may request READ,
     * WRITE, or both (e.g., HTTP/2 connections need simultaneous read+write).
     */
    KlKqueueState *st = loop->_backend;
    struct kevent changes[2];

    int hup = kl_hup_wanted(mask);   /* see kl_event_add_builtin */
    if (kq_set_hup(loop, st, fd, hup) < 0) return -1;
    unsigned short read_flags = ((mask & KL_EVENT_READ) || hup)
        ? (EV_ADD | EV_ENABLE | EV_CLEAR)
        : (EV_ADD | EV_DISABLE);
    unsigned short write_flags = (mask & KL_EVENT_WRITE)
        ? (EV_ADD | EV_ENABLE | EV_CLEAR)
        : (EV_ADD | EV_DISABLE);

    EV_SET(&changes[0], fd, EVFILT_READ, read_flags, 0, 0, udata);
    EV_SET(&changes[1], fd, EVFILT_WRITE, write_flags, 0, 0, udata);

    return kevent(st->fd, changes, 2, NULL, 0, NULL) < 0 ? -1 : 0;
}

int kl_event_del_builtin(KlEventLoop *loop, KlSocketHandle fd) {
    KlKqueueState *st = loop->_backend;
    struct kevent changes[2];
    (void)kq_set_hup(loop, st, fd, 0);
    /* EV_RECEIPT: kevent processes EVERY change and reports each one's result. Without it (and with
     * no event list) it stops at the first failing change, so a registration with only one filter
     * (a WRITE-only watcher has no EVFILT_READ: ENOENT) kept its other filter, which went on
     * delivering events with a udata the caller had freed. A missing filter is expected; 0 if at least
     * one filter was removed and nothing else failed. */
    struct kevent res[2];
    EV_SET(&changes[0], fd, EVFILT_READ, EV_DELETE | EV_RECEIPT, 0, 0, NULL);
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE | EV_RECEIPT, 0, 0, NULL);
    int n = kevent(st->fd, changes, 2, res, 2, NULL);
    if (n < 0) return -1;
    int removed = 0, failed = 0;
    for (int i = 0; i < n; i++) {
        if (!(res[i].flags & EV_ERROR) || res[i].data == 0) removed++;
        else if (res[i].data != ENOENT) failed++;
    }
    return (removed > 0 && !failed) ? 0 : -1;
}

int kl_event_wait_builtin(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    KlKqueueState *st = loop->_backend;
    struct kevent events[KL_EVENT_BATCH];
    int batch = max < KL_EVENT_BATCH ? max : KL_EVENT_BATCH;
    struct timespec ts;
    struct timespec *tsp = NULL;

    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }

    int n = kevent(st->fd, NULL, 0, events, batch, tsp);
    if (n < 0) return errno == EINTR ? 0 : -1;   /* a signal interrupted the wait: an empty tick */

    int count = 0;
    for (int i = 0; i < n; i++) {
        if (events[i].filter == EVFILT_READ && kq_is_hup(st, events[i].ident)) {
            /* A hang-up registration. EV_EOF: the peer's FIN, or a reset (an errno in fflags). A
             * report for data alone is swallowed (EV_CLEAR: one per arrival, no spin). */
            if (!(events[i].flags & EV_EOF)) continue;
            out[count].udata = events[i].udata;
            out[count].ready = KL_EVENT_HANGUP | (events[i].fflags ? KL_EVENT_ERROR : 0);
            count++;
            continue;
        }
        out[count].udata = events[i].udata;
        out[count].ready = 0;
        if (events[i].filter == EVFILT_READ)
            out[count].ready |= KL_EVENT_READ;
        if (events[i].filter == EVFILT_WRITE)
            out[count].ready |= KL_EVENT_WRITE;
        count++;
    }

    return count;
}

void kl_event_close_builtin(KlEventLoop *loop) {
    KlKqueueState *st = loop->_backend;
    if (!st) return;                            /* idempotent */
    if (st->fd >= 0) close(st->fd);             /* close any descriptor, including 0 */
    if (st->hup) kl_free(loop->alloc, st->hup, st->hup_cap);
    kl_free(loop->alloc, st, sizeof(*st));
    loop->_backend = NULL;                      /* reset: resource released exactly once */
}

/* kqueue is a readiness poller of native OS descriptors. */
unsigned kl_event_caps_builtin(const KlEventLoop *loop) {
    (void)loop;
    return KL_EVENT_CAP_READINESS | KL_EVENT_CAP_NATIVE_FD | KL_EVENT_CAP_HANGUP;
}

/* Readiness loop: the default POSIX provider works; nothing to auto-wire. */
const struct KlSocketProvider *kl_event_native_provider_builtin(const KlEventLoop *loop) {
    (void)loop;
    return NULL;
}
