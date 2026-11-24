/*
 * event_winselect.c: Windows readiness event backend (Winsock select()).
 *
 * An INDEPENDENT backend TU, sibling to event_epoll.c / event_kqueue.c / event_poll.c; one is
 * selected by the Makefile per build (the Windows readiness default; BACKEND=winselect, and the old
 * name BACKEND=wsapoll, select it). This file is compiled only on Windows: no cross-platform #ifdef.
 *
 * Why select() and not WSAPoll: after a peer half-close (FIN), WSAPoll reports POLLHUP on every call
 * whatever interest is requested, so a WSAPoll wait cannot block until a half-closed socket becomes
 * writable again: it returns at once, and a writer waiting for room spins or stops. select() keeps
 * write readiness independent of the peer's direction: a full send path is not reported, and room is
 * reported once the peer reads (measured on Windows 10.0.26300; see
 * docs/architecture/windows_readiness_backend.md). Write progress is a readiness invariant, so it
 * takes precedence.
 *
 * What select() cannot express: a hang-up-only watch. It reports a FIN, a half-close and a reset only
 * as readability, exactly like data, and never in the except set. So this backend does NOT advertise
 * KL_EVENT_CAP_HANGUP: a KL_EVENT_HANGUP interest is accepted and never reported, and consumers fall
 * back as on any loop without the capability (the HTTP server's suspended-connection sweep).
 *
 * The descriptor sets are sized at run time to the registrations (the Winsock fd_set is a count plus
 * an array of SOCKETs, read up to fd_count), so no registration is ever dropped because a fixed
 * FD_SETSIZE ran out: an add that cannot grow them fails, never silently.
 *
 * A Winsock SOCKET is a large, sparse handle, so slots are found by a linear scan: O(n) per
 * add/mod/del and per ready socket in a wait, in line with Keel's O(n) router and timeout sweep.
 */

#include <keel/event.h>
#include "event_builtin.h"
#include "event_caps.h"

#include "sockcompat.h"
#include "platform_socket.h"   /* kl_plat_socket_runtime_init: the PAL socket-runtime invariant */
#include <limits.h>
#include <stddef.h>
#include <string.h>

#define INITIAL_CAP 64   /* = FD_SETSIZE: every set allocation is at least sizeof(fd_set) */

/* The Winsock fd_set layout (a count, then the sockets), sized at run time. */
typedef struct {
    u_int  fd_count;
    SOCKET fd_array[1];
} KlWinFdSet;

typedef struct {
    KlAllocator *alloc;
    SOCKET      *sock;    /* registered sockets */
    KlEventMask *mask;    /* their interest */
    void       **udata;   /* parallel udata */
    unsigned char *dead;  /* 1 = select() rejected the handle (closed without kl_event_del) */
    unsigned char *xoff;  /* 1 = the except set proved to report only urgent data here: stop watching it */
    int          count;   /* active entries */
    int          capacity;/* allocated entries (also the capacity of each set) */
    KlWinFdSet  *rset, *wset, *xset;
} KlWinSelState;

static size_t set_bytes(int cap) {
    return offsetof(KlWinFdSet, fd_array) + (size_t)cap * sizeof(SOCKET);
}

static int find_slot(const KlWinSelState *st, KlSocketHandle fd) {
    for (int i = 0; i < st->count; i++)
        if (st->sock[i] == (SOCKET)fd)
            return i;
    return -1;
}

/* Grow every per-registration array and the three sets to `cap` entries; all or nothing. */
static int resize(KlWinSelState *st, int cap) {
    KlAllocator *a = st->alloc;
    const int old = st->capacity;
    SOCKET *ns = kl_malloc(a, (size_t)cap * sizeof(SOCKET));
    KlEventMask *nm = kl_malloc(a, (size_t)cap * sizeof(KlEventMask));
    void **nu = kl_malloc(a, (size_t)cap * sizeof(void *));
    unsigned char *nd = kl_malloc(a, (size_t)cap);
    unsigned char *nx = kl_malloc(a, (size_t)cap);
    KlWinFdSet *r = kl_malloc(a, set_bytes(cap));
    KlWinFdSet *w = kl_malloc(a, set_bytes(cap));
    KlWinFdSet *x = kl_malloc(a, set_bytes(cap));
    if (!ns || !nm || !nu || !nd || !nx || !r || !w || !x) {
        if (ns) kl_free(a, ns, (size_t)cap * sizeof(SOCKET));
        if (nm) kl_free(a, nm, (size_t)cap * sizeof(KlEventMask));
        if (nu) kl_free(a, nu, (size_t)cap * sizeof(void *));
        if (nd) kl_free(a, nd, (size_t)cap);
        if (nx) kl_free(a, nx, (size_t)cap);
        if (r) kl_free(a, r, set_bytes(cap));
        if (w) kl_free(a, w, set_bytes(cap));
        if (x) kl_free(a, x, set_bytes(cap));
        return -1;
    }
    if (st->count > 0) {
        memcpy(ns, st->sock, (size_t)st->count * sizeof(SOCKET));
        memcpy(nm, st->mask, (size_t)st->count * sizeof(KlEventMask));
        memcpy(nu, st->udata, (size_t)st->count * sizeof(void *));
        memcpy(nd, st->dead, (size_t)st->count);
        memcpy(nx, st->xoff, (size_t)st->count);
    }
    if (old > 0) {
        kl_free(a, st->sock, (size_t)old * sizeof(SOCKET));
        kl_free(a, st->mask, (size_t)old * sizeof(KlEventMask));
        kl_free(a, st->udata, (size_t)old * sizeof(void *));
        kl_free(a, st->dead, (size_t)old);
        kl_free(a, st->xoff, (size_t)old);
        kl_free(a, st->rset, set_bytes(old));
        kl_free(a, st->wset, set_bytes(old));
        kl_free(a, st->xset, set_bytes(old));
    }
    st->sock = ns; st->mask = nm; st->udata = nu; st->dead = nd; st->xoff = nx;
    st->rset = r; st->wset = w; st->xset = x;
    st->capacity = cap;
    return 0;
}

int kl_event_init_builtin(KlEventLoop *loop) {
    if (kl_plat_socket_runtime_init() != 0) return -1;   /* PAL invariant */
    KlAllocator *alloc = loop->alloc;
    KlWinSelState *st = kl_malloc(alloc, sizeof(*st));
    if (!st)
        return -1;
    memset(st, 0, sizeof(*st));
    st->alloc = alloc;
    if (resize(st, INITIAL_CAP) < 0) {
        kl_free(alloc, st, sizeof(*st));
        return -1;
    }
    loop->_backend = st;
    return 0;
}

/* PAL-gate: dominated-by kl_event_init_builtin
 * Every other function here operates on the KlWinSelState that kl_event_init_builtin allocated, so
 * reaching one means the PAL gate in that function has already run in this process. The domination is
 * local to this TU, which is the only kind check_winsock_init.pl will accept. */
int kl_event_add_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    KlWinSelState *st = loop->_backend;
    if (!kl_handle_valid(fd))
        return -1;

    /* Idempotent re-add: update in place (matches the other backends' upsert). */
    int existing = find_slot(st, fd);
    if (existing >= 0) {
        st->mask[existing] = mask;
        st->udata[existing] = udata;
        st->dead[existing] = 0;
        st->xoff[existing] = 0;   /* may be a new socket behind a reused handle */
        return 0;
    }
    if (st->count >= st->capacity) {
        if (st->capacity > INT_MAX / 2 || resize(st, st->capacity * 2) < 0)
            return -1;   /* never a silent drop: the caller sees the failure */
    }
    int idx = st->count++;
    st->sock[idx] = (SOCKET)fd;
    st->mask[idx] = mask;
    st->udata[idx] = udata;
    st->dead[idx] = 0;
    st->xoff[idx] = 0;
    return 0;
}

int kl_event_mod_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    KlWinSelState *st = loop->_backend;
    int idx = find_slot(st, fd);
    if (idx < 0)
        return -1;
    st->mask[idx] = mask;
    st->udata[idx] = udata;
    /* kl_watcher_add on a descriptor already in the watcher list arrives here, and Winsock reuses
     * handle values quickly: a registration select() rejected before may be a new socket now. If it
     * is still invalid, the next WSAENOTSOCK marks it again. A new socket starts with the except
     * set watched again. Known limit: a socket closed without kl_event_del whose handle value is
     * reused before any wait sees WSAENOTSOCK is indistinguishable from a re-arm, so it keeps the
     * old registration's xoff; if that new socket is connecting, a failed connect then surfaces
     * only at the caller's deadline. That needs the del-before-close contract to be broken;
     * clearing xoff on every mod instead would cost a wakeup per re-arm on a socket with urgent
     * data pending. */
    if (st->dead[idx]) {
        st->dead[idx] = 0;
        st->xoff[idx] = 0;
    }
    return 0;
}

int kl_event_del_builtin(KlEventLoop *loop, KlSocketHandle fd) {
    KlWinSelState *st = loop->_backend;
    int idx = find_slot(st, fd);
    if (idx < 0)
        return -1;
    int last = st->count - 1;   /* swap with the last entry to keep the arrays compact */
    if (idx != last) {
        st->sock[idx] = st->sock[last];
        st->mask[idx] = st->mask[last];
        st->udata[idx] = st->udata[last];
        st->dead[idx] = st->dead[last];
        st->xoff[idx] = st->xoff[last];
    }
    st->count--;
    return 0;
}

static int in_set(const KlWinFdSet *set, SOCKET s) {
    for (u_int i = 0; i < set->fd_count; i++)
        if (set->fd_array[i] == s)
            return 1;
    return 0;
}

/* select() failed with WSAENOTSOCK: a registered socket was closed without kl_event_del. Mark every
 * handle the stack no longer knows as dead (left out of the sets and never reported, as WSAPoll's
 * POLLNVAL was), so one stale registration cannot stop the loop. Returns how many were found. */
static int mark_dead(KlWinSelState *st) {
    int found = 0;
    for (int i = 0; i < st->count; i++) {
        if (st->dead[i]) continue;
        int type = 0, len = (int)sizeof(type);
        if (getsockopt(st->sock[i], SOL_SOCKET, SO_TYPE, (char *)&type, &len) != 0 &&
            WSAGetLastError() == WSAENOTSOCK) {
            st->dead[i] = 1;
            found++;
        }
    }
    return found;
}

int kl_event_wait_builtin(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    KlWinSelState *st = loop->_backend;

    for (int attempt = 0; attempt < 2; attempt++) {
        st->rset->fd_count = st->wset->fd_count = st->xset->fd_count = 0;
        for (int i = 0; i < st->count; i++) {
            if (st->dead[i]) continue;
            const SOCKET s = st->sock[i];
            if (st->mask[i] & KL_EVENT_READ)
                st->rset->fd_array[st->rset->fd_count++] = s;
            if (st->mask[i] & KL_EVENT_WRITE) {
                st->wset->fd_array[st->wset->fd_count++] = s;
                /* The except set reports a failed connect (SO_ERROR set), and also pending TCP
                 * urgent (OOB) data, which Keel never reads. See the classification below. */
                if (!st->xoff[i])
                    st->xset->fd_array[st->xset->fd_count++] = s;
            }
            /* KL_EVENT_HANGUP alone: not expressible in select() (see the file comment); no set. */
        }
        if (st->rset->fd_count == 0 && st->wset->fd_count == 0) {
            /* select() rejects three empty sets; nothing to wait on, so honor the timeout (an
             * infinite one blocks, as epoll and kqueue do: returning at once would spin a caller). */
            if (timeout_ms != 0) Sleep(timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
            return 0;
        }

        struct timeval tv, *tvp = NULL;
        if (timeout_ms >= 0) {
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            tvp = &tv;
        }
        int n = select(0, st->rset->fd_count ? (fd_set *)st->rset : NULL,
                       st->wset->fd_count ? (fd_set *)st->wset : NULL,
                       st->xset->fd_count ? (fd_set *)st->xset : NULL, tvp);
        if (n == SOCKET_ERROR) {
            if (attempt == 0 && WSAGetLastError() == WSAENOTSOCK && mark_dead(st) > 0)
                continue;   /* rebuild the sets without the stale handles */
            /* Translate so the caller's post-`-1` errno check works (select leaves errno alone). */
            kl_wsa_set_errno();
            return -1;
        }
        if (n == 0)
            return 0;

        /* select() rewrote each set to its ready sockets: map them back to registrations. */
        int count = 0;
        for (int i = 0; i < st->count && count < max; i++) {
            if (st->dead[i]) continue;
            const SOCKET s = st->sock[i];
            const KlEventMask m = st->mask[i];
            KlEventMask ready = 0;
            if ((m & KL_EVENT_READ) && in_set(st->rset, s))
                ready |= KL_EVENT_READ;
            if (m & KL_EVENT_WRITE) {
                if (in_set(st->wset, s)) {
                    ready |= KL_EVENT_WRITE;
                } else if (in_set(st->xset, s)) {
                    /* Classify the exceptional condition that actually fired, with one documented
                     * query and no socket-state tracking: a pending error (a failed connect, or any
                     * socket error) is reported as WRITE so the next operation sees it; with no
                     * error, all the except set can mean is urgent data, which Keel never reads and
                     * which would stay set: stop watching the except set for this registration (a
                     * spin otherwise), until its identity changes (add, upsert, reused handle).
                     * Nothing is lost by that: Winsock's except set reports only a failed connect
                     * and urgent data, urgent data arrives only on an established connection, and
                     * an established connection cannot fail to connect later. Its errors (a reset)
                     * surface through the write and read sets. Reading SO_ERROR does not clear it
                     * on Winsock, so the caller still reads the failure after this report. */
                    int soerr = 0, len = (int)sizeof(soerr);
                    if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &len) != 0 || soerr != 0)
                        ready |= KL_EVENT_WRITE;
                    else
                        st->xoff[i] = 1;
                }
            }
            if (ready) {
                out[count].udata = st->udata[i];
                out[count].ready = ready;
                count++;
            }
        }
        return count;
    }
    return 0;
}

void kl_event_close_builtin(KlEventLoop *loop) {
    KlWinSelState *st = loop->_backend;
    if (!st)
        return;
    KlAllocator *alloc = st->alloc;
    const int cap = st->capacity;
    kl_free(alloc, st->sock, (size_t)cap * sizeof(SOCKET));
    kl_free(alloc, st->mask, (size_t)cap * sizeof(KlEventMask));
    kl_free(alloc, st->udata, (size_t)cap * sizeof(void *));
    kl_free(alloc, st->dead, (size_t)cap);
    kl_free(alloc, st->xoff, (size_t)cap);
    kl_free(alloc, st->rset, set_bytes(cap));
    kl_free(alloc, st->wset, set_bytes(cap));
    kl_free(alloc, st->xset, set_bytes(cap));
    kl_free(alloc, st, sizeof(*st));
    loop->_backend = NULL;
}

/* A readiness poller of native OS descriptors (SOCKETs). No KL_EVENT_CAP_HANGUP: select() reports a
 * peer's FIN only as readability, like data (see the file comment). */
unsigned kl_event_caps_builtin(const KlEventLoop *loop) {
    (void)loop;
    return KL_EVENT_CAP_READINESS | KL_EVENT_CAP_NATIVE_FD;
}

/* Readiness loop: the default Winsock provider works; nothing to auto-wire. */
const struct KlSocketProvider *kl_event_native_provider_builtin(const KlEventLoop *loop) {
    (void)loop;
    return NULL;
}
