#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* POLLRDHUP (glibc exposes it only under _GNU_SOURCE) */
#endif
#include <keel/event.h>
#include "event_builtin.h"
#include "event_caps.h"
#include "event_hangup.h"
#include "event_turn.h"
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <string.h>

/* KL_EVENT_HANGUP needs POLLRDHUP: without it poll() reports nothing for a FIN unless asked for
 * readable data (measured on macOS). Native Linux only: a Cosmopolitan build resolves poll constants
 * per host at run time, so it does not claim the capability and the server uses its sweep there. */
#if defined(__linux__) && !defined(__COSMOPOLITAN__) && defined(POLLRDHUP)
#define KL_POLL_HANGUP 1
#else
#define KL_POLL_HANGUP 0
#endif

#define INITIAL_CAP 64

typedef struct {
    KlAllocator *alloc;
    struct pollfd *fds;   /* pollfd array for poll() */
    void **udata;         /* parallel udata array */
    int count;            /* active entries */
    int capacity;         /* allocated size */
    int *fd_to_idx;       /* fd -> index in fds/udata arrays */
    int fd_to_idx_cap;    /* size of fd_to_idx */
    KlEventTurn *turn;    /* parallel fairness stamps (event_turn.h) */
    KlEventTurns turns;   /* fairness: the current lap and wait sequence */
} KlPollState;

static short mask_to_poll(KlEventMask mask) {
    short events = 0;
    if (mask & KL_EVENT_READ)  events |= POLLIN;
    if (mask & KL_EVENT_WRITE) events |= POLLOUT;
#if KL_POLL_HANGUP
    if (kl_hup_wanted(mask))   events |= POLLRDHUP;
#endif
    return events;
}

#if KL_POLL_HANGUP
/* A hang-up registration asks for POLLRDHUP and not POLLIN. */
static int poll_is_hup(short events) {
    return (events & POLLRDHUP) && !(events & POLLIN);
}
#endif

static int ensure_fd_cap(KlPollState *st, int fd) {
    if (fd < 0)
        return -1;
    if (fd < st->fd_to_idx_cap)
        return 0;

    int new_cap = st->fd_to_idx_cap;
    if (new_cap == 0) new_cap = 16;
    while (new_cap <= fd) {
        if (new_cap > INT_MAX / 2)
            return -1;
        new_cap *= 2;
    }

    size_t old_size = (size_t)st->fd_to_idx_cap * sizeof(int);
    size_t new_size = (size_t)new_cap * sizeof(int);
    int *new_arr = kl_realloc(st->alloc, st->fd_to_idx, old_size, new_size);
    if (!new_arr)
        return -1;

    for (int i = st->fd_to_idx_cap; i < new_cap; i++)
        new_arr[i] = -1;
    st->fd_to_idx = new_arr;
    st->fd_to_idx_cap = new_cap;
    return 0;
}

/* Double the per-registration arrays; all or nothing (each is copied into a fresh allocation, and the
 * old ones are freed only once every new one exists). */
static int grow_arrays(KlPollState *st) {
    int new_cap = st->capacity;
    if (new_cap > INT_MAX / 2)
        return -1;
    new_cap *= 2;
    KlAllocator *a = st->alloc;
    struct pollfd *nf = kl_malloc(a, (size_t)new_cap * sizeof(struct pollfd));
    void **nu = kl_malloc(a, (size_t)new_cap * sizeof(void *));
    KlEventTurn *nt = kl_malloc(a, (size_t)new_cap * sizeof(KlEventTurn));
    if (!nf || !nu || !nt) {
        if (nf) kl_free(a, nf, (size_t)new_cap * sizeof(struct pollfd));
        if (nu) kl_free(a, nu, (size_t)new_cap * sizeof(void *));
        if (nt) kl_free(a, nt, (size_t)new_cap * sizeof(KlEventTurn));
        return -1;
    }
    memcpy(nf, st->fds, (size_t)st->count * sizeof(struct pollfd));
    memcpy(nu, st->udata, (size_t)st->count * sizeof(void *));
    memcpy(nt, st->turn, (size_t)st->count * sizeof(KlEventTurn));
    kl_free(a, st->fds, (size_t)st->capacity * sizeof(struct pollfd));
    kl_free(a, st->udata, (size_t)st->capacity * sizeof(void *));
    kl_free(a, st->turn, (size_t)st->capacity * sizeof(KlEventTurn));
    st->fds = nf;
    st->udata = nu;
    st->turn = nt;
    st->capacity = new_cap;
    return 0;
}

int kl_event_init_builtin(KlEventLoop *loop) {
    KlAllocator *alloc = loop->alloc;
    KlPollState *st = kl_malloc(alloc, sizeof(*st));
    if (!st)
        return -1;
    memset(st, 0, sizeof(*st));
    st->alloc = alloc;

    st->fds = kl_malloc(alloc, INITIAL_CAP * sizeof(struct pollfd));
    if (!st->fds) {
        kl_free(alloc, st, sizeof(*st));
        return -1;
    }

    st->udata = kl_malloc(alloc, INITIAL_CAP * sizeof(void *));
    if (!st->udata) {
        kl_free(alloc, st->fds, INITIAL_CAP * sizeof(struct pollfd));
        kl_free(alloc, st, sizeof(*st));
        return -1;
    }

    st->turn = kl_malloc(alloc, INITIAL_CAP * sizeof(KlEventTurn));
    st->fd_to_idx = st->turn ? kl_malloc(alloc, INITIAL_CAP * sizeof(int)) : NULL;
    if (!st->fd_to_idx) {
        if (st->turn) kl_free(alloc, st->turn, INITIAL_CAP * sizeof(KlEventTurn));
        kl_free(alloc, st->udata, INITIAL_CAP * sizeof(void *));
        kl_free(alloc, st->fds, INITIAL_CAP * sizeof(struct pollfd));
        kl_free(alloc, st, sizeof(*st));
        return -1;
    }

    for (int i = 0; i < INITIAL_CAP; i++)
        st->fd_to_idx[i] = -1;

    st->capacity = INITIAL_CAP;
    st->fd_to_idx_cap = INITIAL_CAP;
    st->count = 0;

    loop->_backend = st;
    return 0;
}

int kl_event_add_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    KlPollState *st = loop->_backend;

    if (ensure_fd_cap(st, fd) < 0)
        return -1;

    /* Idempotent re-add: if fd is already registered, update the existing
     * slot in place instead of appending a duplicate.  Appending would
     * orphan the first slot (leaving a stale fd in poll()) and corrupt the
     * swap-compaction in kl_event_del.  This matches the upsert semantics of
     * the epoll (EPOLL_CTL_ADD → EEXIST) and kqueue (EV_ADD) backends. */
    if (st->fd_to_idx[fd] >= 0) {
        int existing = st->fd_to_idx[fd];
        st->fds[existing].events = mask_to_poll(mask);
        st->fds[existing].revents = 0;
        st->udata[existing] = udata;
        return 0;
    }

    if (st->count >= st->capacity) {
        if (grow_arrays(st) < 0)
            return -1;
    }

    int idx = st->count++;
    st->fds[idx].fd = fd;
    st->fds[idx].events = mask_to_poll(mask);
    st->fds[idx].revents = 0;
    st->udata[idx] = udata;
    kl_turn_join(&st->turns, &st->turn[idx]);
    st->fd_to_idx[fd] = idx;
    return 0;
}

int kl_event_mod_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    KlPollState *st = loop->_backend;

    if (fd < 0 || fd >= st->fd_to_idx_cap)
        return -1;
    int idx = st->fd_to_idx[fd];
    if (idx < 0)
        return -1;

    st->fds[idx].events = mask_to_poll(mask);
    st->udata[idx] = udata;
    return 0;
}

int kl_event_del_builtin(KlEventLoop *loop, KlSocketHandle fd) {
    KlPollState *st = loop->_backend;

    if (fd < 0 || fd >= st->fd_to_idx_cap)
        return -1;
    int idx = st->fd_to_idx[fd];
    if (idx < 0)
        return -1;

    st->fd_to_idx[fd] = -1;

    /* Swap with the last entry to keep the arrays compact. Its fairness stamp goes with it, so the
     * move does not change when it is next reported (event_turn.h). */
    int last = st->count - 1;
    if (idx != last) {
        st->fds[idx] = st->fds[last];
        st->udata[idx] = st->udata[last];
        st->turn[idx] = st->turn[last];
        st->fd_to_idx[st->fds[idx].fd] = idx;
    }
    st->count--;
    return 0;
}

static KlEventMask slot_ready(const KlPollState *st, int i) {
    short rev = st->fds[i].revents;
    if (!rev)
        return 0;
    KlEventMask ready = 0;
#if KL_POLL_HANGUP
    if (poll_is_hup(st->fds[i].events)) {
        /* Level-triggered: reported on every wait until the consumer changes the registration. */
        return kl_hup_ready(rev & POLLRDHUP, rev & POLLHUP, rev & POLLERR, 1);
    }
#endif
    if (rev & (POLLIN | POLLHUP | POLLERR))
        ready |= KL_EVENT_READ;
    if (rev & POLLOUT)
        ready |= KL_EVENT_WRITE;
    return ready;
}

/* Fairness: at most `max` of the ready registrations are reported, picked by laps (event_turn.h),
 * not by position. Scanning from slot 0 and stopping at `max` let the first `max` continuously ready
 * registrations take every wait, and the rest (later connections, a thread-pool wakeup) were never
 * reported. */
int kl_event_wait_builtin(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    KlPollState *st = loop->_backend;

    int n = poll(st->fds, (nfds_t)st->count, timeout_ms);
    if (n < 0)
        return errno == EINTR ? 0 : -1;   /* a signal interrupted the wait: an empty tick */
    if (n == 0)
        return 0;

    kl_turns_begin(&st->turns);
    int count = 0;
    for (int pass = 0; pass < 2 && count < max; pass++) {
        if (pass == 1)
            kl_turns_next_lap(&st->turns);   /* nothing due was left ready: the lap is over */
        for (int i = 0; i < st->count && count < max; i++) {
            if (!kl_turn_due(&st->turns, &st->turn[i]))
                continue;
            KlEventMask ready = slot_ready(st, i);
            if (!ready)
                continue;
            out[count].udata = st->udata[i];
            out[count].ready = ready;
            count++;
            kl_turn_take(&st->turns, &st->turn[i]);
        }
    }
    return count;
}

void kl_event_close_builtin(KlEventLoop *loop) {
    KlPollState *st = loop->_backend;
    if (!st)
        return;

    KlAllocator *alloc = st->alloc;
    kl_free(alloc, st->fd_to_idx, (size_t)st->fd_to_idx_cap * sizeof(int));
    kl_free(alloc, st->udata, (size_t)st->capacity * sizeof(void *));
    kl_free(alloc, st->turn, (size_t)st->capacity * sizeof(KlEventTurn));
    kl_free(alloc, st->fds, (size_t)st->capacity * sizeof(struct pollfd));
    kl_free(alloc, st, sizeof(*st));
    loop->_backend = NULL;
}

/* poll() is a readiness poller of native OS descriptors. */
unsigned kl_event_caps_builtin(const KlEventLoop *loop) {
    (void)loop;
    return KL_EVENT_CAP_READINESS | KL_EVENT_CAP_NATIVE_FD |
           (KL_POLL_HANGUP ? KL_EVENT_CAP_HANGUP : 0u);
}

/* Readiness loop: the default POSIX provider works; nothing to auto-wire. */
const struct KlSocketProvider *kl_event_native_provider_builtin(const KlEventLoop *loop) {
    (void)loop;
    return NULL;
}
