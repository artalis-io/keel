#include <keel/event.h>
#include <liburing.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define RING_SIZE 256
#define INITIAL_FD_CAP 256

/*
 * Sentinel value for "fd slot is unused."  Distinct from NULL, which is a
 * valid udata value (the listen socket uses NULL to identify itself).
 * Pointer value 1 can never be returned by malloc on any architecture.
 */
#define UDATA_UNUSED ((void *)(uintptr_t)1)

typedef struct {
    struct io_uring ring;
    KlAllocator *alloc;
    void **fd_udata;    /* fd → udata mapping (flat array, indexed by fd) */
    int fd_udata_cap;
    int pending;        /* SQEs queued but not yet submitted */
} KlIoUringState;

static int ensure_fd_cap(KlIoUringState *st, int fd) {
    if (fd < st->fd_udata_cap)
        return 0;

    int new_cap = st->fd_udata_cap;
    if (new_cap == 0) new_cap = 16;
    while (new_cap <= fd) {
        if (new_cap > INT_MAX / 2)
            return -1;
        new_cap *= 2;
    }

    size_t old_size = (size_t)st->fd_udata_cap * sizeof(void *);
    size_t new_size = (size_t)new_cap * sizeof(void *);
    void **new_arr = kl_realloc(st->alloc, st->fd_udata, old_size, new_size);
    if (!new_arr)
        return -1;

    for (int i = st->fd_udata_cap; i < new_cap; i++)
        new_arr[i] = UDATA_UNUSED;
    st->fd_udata = new_arr;
    st->fd_udata_cap = new_cap;
    return 0;
}

static unsigned mask_to_poll(KlEventMask mask) {
    unsigned events = 0;
    if (mask & KL_EVENT_READ)  events |= POLLIN;
    if (mask & KL_EVENT_WRITE) events |= POLLOUT;
    return events;
}

int kl_event_init(KlEventLoop *loop) {
    KlAllocator *alloc = loop->alloc;
    KlIoUringState *st = kl_malloc(alloc, sizeof(*st));
    if (!st)
        return -1;
    memset(st, 0, sizeof(*st));
    st->alloc = alloc;

    if (io_uring_queue_init(RING_SIZE, &st->ring, 0) < 0) {
        kl_free(alloc, st, sizeof(*st));
        return -1;
    }

    size_t udata_size = INITIAL_FD_CAP * sizeof(void *);
    st->fd_udata = kl_malloc(alloc, udata_size);
    if (!st->fd_udata) {
        io_uring_queue_exit(&st->ring);
        kl_free(alloc, st, sizeof(*st));
        return -1;
    }
    for (int i = 0; i < INITIAL_FD_CAP; i++)
        st->fd_udata[i] = UDATA_UNUSED;
    st->fd_udata_cap = INITIAL_FD_CAP;
    st->pending = 0;

    loop->fd = -1;
    loop->_backend = st;
    return 0;
}

int kl_event_add(KlEventLoop *loop, int fd, KlEventMask mask, void *udata) {
    KlIoUringState *st = loop->_backend;

    if (ensure_fd_cap(st, fd) < 0)
        return -1;

    st->fd_udata[fd] = udata;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&st->ring);
    if (!sqe)
        return -1;

    io_uring_prep_poll_add(sqe, fd, mask_to_poll(mask));
    io_uring_sqe_set_data64(sqe, (uint64_t)fd);
    st->pending++;
    return 0;
}

int kl_event_mod(KlEventLoop *loop, int fd, KlEventMask mask, void *udata) {
    KlIoUringState *st = loop->_backend;

    if (ensure_fd_cap(st, fd) < 0)
        return -1;

    st->fd_udata[fd] = udata;

    /* Flush pending SQEs to maximize SQ space before queuing two more */
    if (st->pending > 0) {
        io_uring_submit(&st->ring);
        st->pending = 0;
    }

    /* Cancel existing poll, then add new one */
    struct io_uring_sqe *sqe = io_uring_get_sqe(&st->ring);
    if (!sqe)
        return -1;
    io_uring_prep_poll_remove(sqe, (uint64_t)fd);
    io_uring_sqe_set_data64(sqe, (uint64_t)-1); /* sentinel: ignore CQE */
    st->pending++;

    sqe = io_uring_get_sqe(&st->ring);
    if (!sqe) {
        /* Cancel already queued — flush it then retry once */
        io_uring_submit(&st->ring);
        st->pending = 0;
        sqe = io_uring_get_sqe(&st->ring);
        if (!sqe)
            return -1;
    }
    io_uring_prep_poll_add(sqe, fd, mask_to_poll(mask));
    io_uring_sqe_set_data64(sqe, (uint64_t)fd);
    st->pending++;
    return 0;
}

int kl_event_del(KlEventLoop *loop, int fd) {
    KlIoUringState *st = loop->_backend;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&st->ring);
    if (!sqe)
        return -1;

    io_uring_prep_poll_remove(sqe, (uint64_t)fd);
    io_uring_sqe_set_data64(sqe, (uint64_t)-1); /* sentinel */
    st->pending++;

    if (fd < st->fd_udata_cap)
        st->fd_udata[fd] = UDATA_UNUSED;

    return 0;
}

int kl_event_wait(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    KlIoUringState *st = loop->_backend;

    /* Submit any pending SQEs */
    if (st->pending > 0) {
        int submitted = io_uring_submit(&st->ring);
        if (submitted < 0)
            return -1;
        st->pending = 0;
    }

    /* Wait for at least one CQE */
    struct io_uring_cqe *cqe;
    struct __kernel_timespec ts;
    struct __kernel_timespec *tsp = NULL;

    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long long)(timeout_ms % 1000) * 1000000LL;
        tsp = &ts;
    }

    int ret = io_uring_wait_cqe_timeout(&st->ring, &cqe, tsp);
    if (ret < 0)
        return ret == -ETIME ? 0 : -1;

    /* Drain available CQEs */
    int count = 0;
    int seen = 0;
    unsigned head;
    io_uring_for_each_cqe(&st->ring, head, cqe) {
        uint64_t fd64 = io_uring_cqe_get_data64(cqe);

        /* Skip sentinel CQEs (from poll_remove) */
        if (fd64 == (uint64_t)-1) {
            seen++;
            continue;
        }

        int fd = (int)fd64;
        if (fd < 0 || fd >= st->fd_udata_cap ||
            st->fd_udata[fd] == UDATA_UNUSED) {
            seen++;
            continue;
        }

        /* Stop before consuming CQEs we can't report */
        if (count >= max)
            break;

        seen++;
        int revents = cqe->res;
        if (revents <= 0)
            continue;  /* Error/cancelled poll (e.g. from poll_remove) */

        KlEventMask ready = 0;
        if (revents & (POLLIN | POLLHUP | POLLERR))
            ready |= KL_EVENT_READ;
        if (revents & POLLOUT)
            ready |= KL_EVENT_WRITE;

        if (ready) {
            out[count].udata = st->fd_udata[fd];
            out[count].ready = ready;
            count++;
        }
    }

    io_uring_cq_advance(&st->ring, (unsigned)seen);
    return count;
}

void kl_event_close(KlEventLoop *loop) {
    KlIoUringState *st = loop->_backend;
    if (!st)
        return;

    KlAllocator *alloc = st->alloc;
    io_uring_queue_exit(&st->ring);
    kl_free(alloc, st->fd_udata,
            (size_t)st->fd_udata_cap * sizeof(void *));
    kl_free(alloc, st, sizeof(*st));
    loop->_backend = NULL;
    loop->fd = -1;
}
