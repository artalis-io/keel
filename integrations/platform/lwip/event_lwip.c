/*
 * event_lwip.c: lwIP readiness event backend over lwip_poll, packaged as a
 * runtime KlEventProvider. The readiness half of the lwIP platform; pair it with
 * socket_lwip.c so both agree on what a "native, pollable" fd is. lwIP socket fds
 * are small dense ints, so this reuses the event_poll.c direct-index model with
 * poll → lwip_poll and struct pollfd / POLL* from lwip/sockets.h.
 *
 * BYO / opt-in: build against your own lwIP (LWIP_DIR). Uses only the public
 * <keel/event.h> + <keel/socket.h> contracts. See docs/archive/designs/lwip_platform_design.md.
 *
 * SPDX-License-Identifier: MIT
 */
#include "keel_lwip.h"

#include <limits.h>
#include <string.h>

#include "lwip/sockets.h"   /* lwip_poll, struct pollfd, POLLIN/OUT/HUP/ERR, nfds_t */

#define INITIAL_CAP 64

typedef struct {
    KlAllocator *alloc;
    struct pollfd *fds;
    void **udata;
    int count;
    int capacity;
    int *fd_to_idx;
    int fd_to_idx_cap;
} KlLwipPollState;

static short mask_to_poll(KlEventMask mask) {
    short events = 0;
    if (mask & KL_EVENT_READ)  events |= POLLIN;
    if (mask & KL_EVENT_WRITE) events |= POLLOUT;
    return events;
}

static int ensure_fd_cap(KlLwipPollState *st, int fd) {
    if (fd < 0) return -1;
    if (fd < st->fd_to_idx_cap) return 0;
    int new_cap = st->fd_to_idx_cap ? st->fd_to_idx_cap : 16;
    while (new_cap <= fd) {
        if (new_cap > INT_MAX / 2) return -1;
        new_cap *= 2;
    }
    size_t old_size = (size_t)st->fd_to_idx_cap * sizeof(int);
    size_t new_size = (size_t)new_cap * sizeof(int);
    int *arr = kl_realloc(st->alloc, st->fd_to_idx, old_size, new_size);
    if (!arr) return -1;
    for (int i = st->fd_to_idx_cap; i < new_cap; i++) arr[i] = -1;
    st->fd_to_idx = arr;
    st->fd_to_idx_cap = new_cap;
    return 0;
}

static int grow_arrays(KlLwipPollState *st) {
    int new_cap = st->capacity;
    if (new_cap > INT_MAX / 2) return -1;
    new_cap *= 2;
    size_t old_fds = (size_t)st->capacity * sizeof(struct pollfd);
    size_t new_fds = (size_t)new_cap * sizeof(struct pollfd);
    struct pollfd *nf = kl_realloc(st->alloc, st->fds, old_fds, new_fds);
    if (!nf) return -1;
    size_t old_ud = (size_t)st->capacity * sizeof(void *);
    size_t new_ud = (size_t)new_cap * sizeof(void *);
    void **nu = kl_realloc(st->alloc, st->udata, old_ud, new_ud);
    if (!nu) {
        struct pollfd *rev = kl_realloc(st->alloc, nf, new_fds, old_fds);
        st->fds = rev ? rev : nf;
        return -1;
    }
    st->fds = nf;
    st->udata = nu;
    st->capacity = new_cap;
    return 0;
}

static int lwev_init(KlEventLoop *loop) {
    KlAllocator *alloc = loop->alloc;
    KlLwipPollState *st = kl_malloc(alloc, sizeof(*st));
    if (!st) return -1;
    memset(st, 0, sizeof(*st));
    st->alloc = alloc;
    st->fds = kl_malloc(alloc, INITIAL_CAP * sizeof(struct pollfd));
    if (!st->fds) { kl_free(alloc, st, sizeof(*st)); return -1; }
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
    for (int i = 0; i < INITIAL_CAP; i++) st->fd_to_idx[i] = -1;
    st->capacity = INITIAL_CAP;
    st->fd_to_idx_cap = INITIAL_CAP;
    loop->_backend = st;
    return 0;
}

static int lwev_add(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    KlLwipPollState *st = loop->_backend;
    if (!kl_handle_valid(fd) || ensure_fd_cap(st, (int)fd) < 0) return -1;
    if (st->fd_to_idx[(int)fd] >= 0) {          /* idempotent upsert */
        int e = st->fd_to_idx[(int)fd];
        st->fds[e].events = mask_to_poll(mask);
        st->fds[e].revents = 0;
        st->udata[e] = udata;
        return 0;
    }
    if (st->count >= st->capacity && grow_arrays(st) < 0) return -1;
    int idx = st->count++;
    st->fds[idx].fd = (int)fd;
    st->fds[idx].events = mask_to_poll(mask);
    st->fds[idx].revents = 0;
    st->udata[idx] = udata;
    st->fd_to_idx[(int)fd] = idx;
    return 0;
}

static int lwev_mod(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    KlLwipPollState *st = loop->_backend;
    /* Reject the full negative range (kl_handle_valid only rejects -1), not just
     * the upper bound; else fd <= -2 indexes fd_to_idx[] out of bounds. */
    if ((int)fd < 0 || (int)fd >= st->fd_to_idx_cap) return -1;
    int idx = st->fd_to_idx[(int)fd];
    if (idx < 0) return -1;
    st->fds[idx].events = mask_to_poll(mask);
    st->udata[idx] = udata;
    return 0;
}

static int lwev_del(KlEventLoop *loop, KlSocketHandle fd) {
    KlLwipPollState *st = loop->_backend;
    /* Reject the full negative range (see lwev_mod), not just the upper bound. */
    if ((int)fd < 0 || (int)fd >= st->fd_to_idx_cap) return -1;
    int idx = st->fd_to_idx[(int)fd];
    if (idx < 0) return -1;
    st->fd_to_idx[(int)fd] = -1;
    int last = st->count - 1;
    if (idx != last) {
        st->fds[idx] = st->fds[last];
        st->udata[idx] = st->udata[last];
        st->fd_to_idx[st->fds[idx].fd] = idx;
    }
    st->count--;
    return 0;
}

static int lwev_wait(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    KlLwipPollState *st = loop->_backend;
    int n = lwip_poll(st->fds, (nfds_t)st->count, timeout_ms);
    if (n < 0) return -1;
    int count = 0;
    for (int i = 0; i < st->count && count < max; i++) {
        short rev = st->fds[i].revents;
        if (!rev) continue;
        KlEventMask ready = 0;
        if (rev & (POLLIN | POLLHUP | POLLERR)) ready |= KL_EVENT_READ;
        if (rev & POLLOUT) ready |= KL_EVENT_WRITE;
        if (ready) { out[count].udata = st->udata[i]; out[count].ready = ready; count++; }
    }
    return count;
}

static void lwev_close(KlEventLoop *loop) {
    KlLwipPollState *st = loop->_backend;
    if (!st) return;
    KlAllocator *alloc = st->alloc;
    kl_free(alloc, st->fd_to_idx, (size_t)st->fd_to_idx_cap * sizeof(int));
    kl_free(alloc, st->udata, (size_t)st->capacity * sizeof(void *));
    kl_free(alloc, st->fds, (size_t)st->capacity * sizeof(struct pollfd));
    kl_free(alloc, st, sizeof(*st));
    loop->_backend = NULL;
}

/* Readiness poller of lwIP-native descriptors. */
static unsigned lwev_caps(const KlEventLoop *loop) {
    (void)loop;
    return KL_EVENT_CAP_READINESS | KL_EVENT_CAP_NATIVE_FD;
}

/* The socket provider this loop must be paired with: lwip_poll can only poll
 * lwIP socket fds, so the loop needs the lwIP socket provider. Returning it here
 * lets the server auto-wire the matched pair when only event_provider is set. */
static const KlSocketProvider *lwev_native_provider(const KlEventLoop *loop) {
    (void)loop;
    return kl_socket_provider_lwip();
}

static const KlEventOps lwip_event_ops = {
    .init = lwev_init, .add = lwev_add, .mod = lwev_mod, .del = lwev_del,
    .wait = lwev_wait, .close = lwev_close, .caps = lwev_caps,
    .native_provider = lwev_native_provider,
    .completion = NULL,   /* readiness provider: no completion axis */
};

static const KlEventProvider lwip_event_provider = { &lwip_event_ops, "lwip" };

const KlEventProvider *kl_event_provider_lwip(void) { return &lwip_event_provider; }
