/*
 * event_wsapoll.c: Windows event backend (WSAPoll).
 *
 * An INDEPENDENT backend TU, sibling to event_epoll.c / event_kqueue.c /
 * event_poll.c; one is selected by the Makefile per build.
 * There is NO cross-platform #ifdef here or in the POSIX backends: this file is
 * compiled only on Windows (see the Makefile OS=windows branch).
 *
 * Unlike event_poll.c, this does not use an fd-indexed slot map: a Winsock
 * SOCKET is a large, sparse kernel handle (not a small dense fd), so the
 * WSAPOLLFD array is scanned linearly to find a socket's slot: O(n) per
 * add/mod/del, in line with Keel's O(n) router / O(n) timeout sweep. See
 * docs/archive/phases/phase6_winsock_design.md §B.2.
 */

#include <keel/event.h>
#include "event_builtin.h"
#include "event_caps.h"
#include "event_hangup.h"

#include "sockcompat.h"
#include "platform_socket.h"   /* kl_plat_socket_runtime_init: the PAL socket-runtime invariant */   /* winsock2.h + kl_wsa_set_errno() */
#include <limits.h>
#include <string.h>

#define INITIAL_CAP 64

typedef struct {
    KlAllocator *alloc;
    WSAPOLLFD   *fds;     /* pollfd array for WSAPoll() */
    void       **udata;   /* parallel udata array */
    unsigned char *hup;   /* parallel: 1 = a hang-up registration (KL_EVENT_HANGUP interest alone).
                           * Decided by interest, never by udata; events = 0 alone is ambiguous with
                           * a paused READ. */
    int          count;   /* active entries */
    int          capacity;/* allocated size */
} KlWsaPollState;

/* WSAPoll input events: POLLRDNORM/POLLWRNORM are the documented settable flags
 * (POLLIN/POLLOUT are output-only aliases in winsock2.h). */
static short mask_to_poll(KlEventMask mask) {
    short events = 0;
    if (mask & KL_EVENT_READ)  events |= POLLRDNORM;
    if (mask & KL_EVENT_WRITE) events |= POLLWRNORM;
    return events;
}

/* Linear scan: index of the slot holding `fd`, or -1. SOCKET is not a dense
 * small int, so no direct-index map (cf. event_poll.c). */
static int find_slot(const KlWsaPollState *st, KlSocketHandle fd) {
    for (int i = 0; i < st->count; i++)
        if (st->fds[i].fd == (SOCKET)fd)
            return i;
    return -1;
}

static int grow_arrays(KlWsaPollState *st) {
    int new_cap = st->capacity;
    if (new_cap > INT_MAX / 2)
        return -1;
    new_cap *= 2;

    size_t old_fds_sz = (size_t)st->capacity * sizeof(WSAPOLLFD);
    size_t new_fds_sz = (size_t)new_cap * sizeof(WSAPOLLFD);
    WSAPOLLFD *nf = kl_realloc(st->alloc, st->fds, old_fds_sz, new_fds_sz);
    if (!nf)
        return -1;

    size_t old_ud_sz = (size_t)st->capacity * sizeof(void *);
    size_t new_ud_sz = (size_t)new_cap * sizeof(void *);
    void **nu = kl_realloc(st->alloc, st->udata, old_ud_sz, new_ud_sz);
    if (!nu) {
        WSAPOLLFD *rev = kl_realloc(st->alloc, nf, new_fds_sz, old_fds_sz);
        st->fds = rev ? rev : nf;
        return -1;
    }

    unsigned char *nh = kl_realloc(st->alloc, st->hup, (size_t)st->capacity, (size_t)new_cap);
    if (!nh) {
        void **ru = kl_realloc(st->alloc, nu, new_ud_sz, old_ud_sz);
        st->udata = ru ? ru : nu;
        WSAPOLLFD *rev = kl_realloc(st->alloc, nf, new_fds_sz, old_fds_sz);
        st->fds = rev ? rev : nf;
        return -1;
    }

    st->fds = nf;
    st->udata = nu;
    st->hup = nh;
    st->capacity = new_cap;
    return 0;
}

int kl_event_init_builtin(KlEventLoop *loop) {
    if (kl_plat_socket_runtime_init() != 0) return -1;   /* PAL invariant */
    KlAllocator *alloc = loop->alloc;
    KlWsaPollState *st = kl_malloc(alloc, sizeof(*st));
    if (!st)
        return -1;
    memset(st, 0, sizeof(*st));
    st->alloc = alloc;

    st->fds = kl_malloc(alloc, INITIAL_CAP * sizeof(WSAPOLLFD));
    if (!st->fds) {
        kl_free(alloc, st, sizeof(*st));
        return -1;
    }
    st->udata = kl_malloc(alloc, INITIAL_CAP * sizeof(void *));
    if (!st->udata) {
        kl_free(alloc, st->fds, INITIAL_CAP * sizeof(WSAPOLLFD));
        kl_free(alloc, st, sizeof(*st));
        return -1;
    }
    st->hup = kl_malloc(alloc, INITIAL_CAP);
    if (!st->hup) {
        kl_free(alloc, st->udata, INITIAL_CAP * sizeof(void *));
        kl_free(alloc, st->fds, INITIAL_CAP * sizeof(WSAPOLLFD));
        kl_free(alloc, st, sizeof(*st));
        return -1;
    }

    st->capacity = INITIAL_CAP;
    st->count = 0;

    loop->_backend = st;
    return 0;
}

/* PAL-gate: dominated-by kl_event_init_builtin
 * Every other function here operates on the KlWsaPollState that kl_event_init_builtin allocated, so
 * reaching one means the PAL gate in that function has already run in this process. The domination is
 * local to this TU, which is the only kind check_winsock_init.pl will accept. */
int kl_event_add_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    KlWsaPollState *st = loop->_backend;

    /* Reject an invalid socket handle up front (matches the epoll/kqueue/poll
     * backends, which reject fd < 0). INVALID_SOCKET is (SOCKET)-1. */
    if (!kl_handle_valid(fd))
        return -1;

    /* A hang-up registration polls with no interest (events = 0): WSAPoll still reports HUP for
     * a FIN and HUP|ERR for a reset, and nothing for data (measured, including on sockets associated
     * with an IOCP port). It is flagged in st->hup, because events = 0 is also a paused READ. */
    unsigned char hup = (unsigned char)kl_hup_wanted(mask);

    /* Idempotent re-add: update in place (matches epoll/kqueue/poll upsert). */
    int existing = find_slot(st, fd);
    if (existing >= 0) {
        st->fds[existing].events = mask_to_poll(mask);
        st->fds[existing].revents = 0;
        st->udata[existing] = udata;
        st->hup[existing] = hup;
        return 0;
    }

    if (st->count >= st->capacity) {
        if (grow_arrays(st) < 0)
            return -1;
    }

    int idx = st->count++;
    st->fds[idx].fd = (SOCKET)fd;
    st->fds[idx].events = mask_to_poll(mask);
    st->fds[idx].revents = 0;
    st->udata[idx] = udata;
    st->hup[idx] = hup;
    return 0;
}

int kl_event_mod_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    KlWsaPollState *st = loop->_backend;
    int idx = find_slot(st, fd);
    if (idx < 0)
        return -1;
    st->fds[idx].events = mask_to_poll(mask);
    st->udata[idx] = udata;
    st->hup[idx] = (unsigned char)kl_hup_wanted(mask);   /* see kl_event_add_builtin */
    return 0;
}

int kl_event_del_builtin(KlEventLoop *loop, KlSocketHandle fd) {
    KlWsaPollState *st = loop->_backend;
    int idx = find_slot(st, fd);
    if (idx < 0)
        return -1;

    /* Swap with last entry to keep the array compact. */
    int last = st->count - 1;
    if (idx != last) {
        st->fds[idx] = st->fds[last];
        st->udata[idx] = st->udata[last];
        st->hup[idx] = st->hup[last];
    }
    st->count--;
    return 0;
}

int kl_event_wait_builtin(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    KlWsaPollState *st = loop->_backend;

    if (st->count == 0) {
        /* WSAPoll requires nfds >= 1; nothing to wait on, so honor the timeout. */
        if (timeout_ms > 0) Sleep((DWORD)timeout_ms);
        return 0;
    }

    int n = WSAPoll(st->fds, (ULONG)st->count, timeout_ms);
    if (n == SOCKET_ERROR) {
        /* Translate so the caller's post-`-1` errno check works; the server's
         * accept loop tests `errno == EINTR` to decide retry-vs-abort; without
         * this it would read a stale errno (WSAPoll leaves errno untouched). */
        kl_wsa_set_errno();
        return -1;
    }

    int count = 0;
    for (int i = 0; i < st->count && count < max; i++) {
        short rev = st->fds[i].revents;
        if (!rev)
            continue;

        KlEventMask ready = 0;
        if (st->hup[i]) {
            /* Hang-up registration, level-triggered: HUP is the FIN, ERR a reset (a FIN alone shows
             * HUP without ERR, so HUP is not a failure here). */
            ready = kl_hup_ready(0, rev & POLLHUP, rev & POLLERR, 0);
        } else {
            if (rev & (POLLRDNORM | POLLHUP | POLLERR))
                ready |= KL_EVENT_READ;
            if (rev & POLLWRNORM)
                ready |= KL_EVENT_WRITE;
        }

        if (ready) {
            out[count].udata = st->udata[i];
            out[count].ready = ready;
            count++;
        }
    }

    return count;
}

void kl_event_close_builtin(KlEventLoop *loop) {
    KlWsaPollState *st = loop->_backend;
    if (!st)
        return;

    KlAllocator *alloc = st->alloc;
    kl_free(alloc, st->hup, (size_t)st->capacity);
    kl_free(alloc, st->udata, (size_t)st->capacity * sizeof(void *));
    kl_free(alloc, st->fds, (size_t)st->capacity * sizeof(WSAPOLLFD));
    kl_free(alloc, st, sizeof(*st));
    loop->_backend = NULL;
}

/* WSAPoll is a readiness poller of native OS descriptors (SOCKETs). */
unsigned kl_event_caps_builtin(const KlEventLoop *loop) {
    (void)loop;
    return KL_EVENT_CAP_READINESS | KL_EVENT_CAP_NATIVE_FD | KL_EVENT_CAP_HANGUP;
}

/* Readiness loop: the default Winsock provider works; nothing to auto-wire. */
const struct KlSocketProvider *kl_event_native_provider_builtin(const KlEventLoop *loop) {
    (void)loop;
    return NULL;
}
