#include <keel/event.h>
#include "event_builtin.h"
#include "event_caps.h"
#include <limits.h>
#include <poll.h>
#include <string.h>

#define INITIAL_CAP 64

typedef struct {
    KlAllocator *alloc;
    struct pollfd *fds;   /* pollfd array for poll() */
    void **udata;         /* parallel udata array */
    int count;            /* active entries */
    int capacity;         /* allocated size */
    int *fd_to_idx;       /* fd -> index in fds/udata arrays */
    int fd_to_idx_cap;    /* size of fd_to_idx */
} KlPollState;

static short mask_to_poll(KlEventMask mask) {
    short events = 0;
    if (mask & KL_EVENT_READ)  events |= POLLIN;
    if (mask & KL_EVENT_WRITE) events |= POLLOUT;
    return events;
}

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

static int grow_arrays(KlPollState *st) {
    int new_cap = st->capacity;
    if (new_cap > INT_MAX / 2)
        return -1;
    new_cap *= 2;

    size_t old_fds_sz = (size_t)st->capacity * sizeof(struct pollfd);
    size_t new_fds_sz = (size_t)new_cap * sizeof(struct pollfd);
    struct pollfd *nf = kl_realloc(st->alloc, st->fds, old_fds_sz, new_fds_sz);
    if (!nf)
        return -1;

    size_t old_ud_sz = (size_t)st->capacity * sizeof(void *);
    size_t new_ud_sz = (size_t)new_cap * sizeof(void *);
    void **nu = kl_realloc(st->alloc, st->udata, old_ud_sz, new_ud_sz);
    if (!nu) {
        /* fds realloc succeeded but udata failed; shrink fds back to
         * keep state consistent with st->capacity for kl_free sizing. */
        struct pollfd *rev = kl_realloc(st->alloc, nf, new_fds_sz, old_fds_sz);
        st->fds = rev ? rev : nf;
        return -1;
    }

    st->fds = nf;
    st->udata = nu;
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

    st->fd_to_idx = kl_malloc(alloc, INITIAL_CAP * sizeof(int));
    if (!st->fd_to_idx) {
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

    /* Swap with last entry to keep array compact */
    int last = st->count - 1;
    if (idx != last) {
        st->fds[idx] = st->fds[last];
        st->udata[idx] = st->udata[last];
        st->fd_to_idx[st->fds[idx].fd] = idx;
    }
    st->count--;
    return 0;
}

int kl_event_wait_builtin(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    KlPollState *st = loop->_backend;

    int n = poll(st->fds, (nfds_t)st->count, timeout_ms);
    if (n < 0)
        return -1;

    int count = 0;
    for (int i = 0; i < st->count && count < max; i++) {
        short rev = st->fds[i].revents;
        if (!rev)
            continue;

        KlEventMask ready = 0;
        if (rev & (POLLIN | POLLHUP | POLLERR))
            ready |= KL_EVENT_READ;
        if (rev & POLLOUT)
            ready |= KL_EVENT_WRITE;

        if (ready) {
            out[count].udata = st->udata[i];
            out[count].ready = ready;
            count++;
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
    kl_free(alloc, st->fds, (size_t)st->capacity * sizeof(struct pollfd));
    kl_free(alloc, st, sizeof(*st));
    loop->_backend = NULL;
}

/* poll() is a readiness poller of native OS descriptors. */
unsigned kl_event_caps_builtin(const KlEventLoop *loop) {
    (void)loop;
    return KL_EVENT_CAP_READINESS | KL_EVENT_CAP_NATIVE_FD;
}

/* Readiness loop: the default POSIX provider works; nothing to auto-wire. */
const struct KlSocketProvider *kl_event_native_provider_builtin(const KlEventLoop *loop) {
    (void)loop;
    return NULL;
}
